// SPDX-License-Identifier: BSD-3-Clause-Clear
/* MT7927 TX execution: affiliated-link choice, TID/BA pinning, primary-handle
 * diagnostics, load/affinity policy and native header handling. Generic skb
 * submission, power management and descriptor/key/PN ownership remain mt76.
 */
#include <linux/etherdevice.h>
#include "../mt7925.h"
#include "../../mt792x_mlo_sched.h"
#include "../mlo_tx_liveness.h"
#include "../mlo_tx_primary.h"
#include "pairs.h"
#include "../mac.h"
#include "../mcu.h"

/* EXPERIMENTAL: host-side override of MLO TX link selection.
 *
 * Normally mt792x_tx() below reads the link a packet should go out on from
 * info->control.flags (IEEE80211_TX_CTRL_MLO_LINK), a value mac80211 itself
 * stamps onto the skb before the driver ever sees it -- the driver has never
 * made this choice. This variable, when >= 0, overrides that choice at the
 * one point in the driver where a link id is turned into a WCID (and
 * therefore a TXD/firmware BSS), so a link the driver -- not mac80211's own
 * scheduler -- picks can be exercised experimentally. Default -1 (off,
 * mac80211's own selection is used unchanged). Set via mt7925 debugfs
 * "mlo_force_tx_link".
 */
int mt7925_mlo_force_tx_link = -1;
EXPORT_SYMBOL_GPL(mt7925_mlo_force_tx_link);

/* EXPERIMENTAL: sentinel for mt7925_mlo_force_tx_link meaning "alternate
 * between link 0 and link 2 on successive bulk-TXQ scheduling decisions"
 * instead of pinning to one link. Wire encoding (debugfs) is 3; internal
 * representation is this distinct negative sentinel so it can never be
 * confused with a real link id (0..14).
 */
#define MT7925_MLO_FORCE_ALTERNATE (-2)
#define MT7925_MLO_FORCE_FLOWHASH (-3)
#define MT7925_MLO_FORCE_CAPHASH (-4)
#define MT7925_MLO_BUCKETS 1024
#define MT7925_MLO_FORCE_LOADAWARE (-5)

/* Load-aware state. Sampling cadence matches mt792x_mac_work
 * (MT792x_WATCHDOG_TIME = HZ/4 = 250ms), which is what refreshes
 * mlink->airtime_ac[] from the WTBL, so sampling faster would just
 * re-read the same values.
 */
struct mt7925_mlo_link_load {
	u64 last_tx_us;      /* previous cumulative TX airtime (us) */
	u32 last_delta_us;   /* most recent delta, for debugfs */
	u32 elapsed_us;
	unsigned long last_ts;
	u8 raw_util;
	u8 ema_util;         /* smoothed utilisation, 0..100 */
	u32 spare_weight;
	u32 final_weight;
};

static struct mt7925_mlo_link_load mt7925_mlo_load[IEEE80211_MLD_MAX_NUM_LINKS];
static atomic64_t mt7925_mlo_placements[IEEE80211_MLD_MAX_NUM_LINKS];
static atomic64_t mt7925_mlo_cache_hits[IEEE80211_MLD_MAX_NUM_LINKS];

/* Bounded, statically allocated flow-affinity cache.
 *
 * Load-aware weights move bucket boundaries, and a moving boundary WILL
 * reassign a live flow -- the stateless hash alone does not preserve
 * affinity once weights change. So an existing flow's link is remembered
 * here and never recomputed while the flow stays active. New flows (cache
 * miss, or an entry aged out) are placed using the current weights.
 *
 * The fixed array is searched by exact hash. A miss uses an invalid slot or
 * replaces the oldest entry, so unrelated low hash bits cannot evict a live
 * flow while the bounded cache still has room.
 */
#define MT7925_MLO_FC_SIZE 256
struct mt7925_mlo_fc_ent {
	u32 hash;
	unsigned long ts;
	u8 link;
	bool valid;
};

static struct mt7925_mlo_fc_ent mt7925_mlo_fc[MT7925_MLO_FC_SIZE];
static DEFINE_SPINLOCK(mt7925_mlo_fc_lock);

static struct mt7925_mlo_fc_ent *
mt7925_mlo_fc_lookup(struct mt7925_mlo_fc_ent *cache, u32 hash)
{
	struct mt7925_mlo_fc_ent *oldest = &cache[0];
	int i;

	for (i = 0; i < MT7925_MLO_FC_SIZE; i++) {
		struct mt7925_mlo_fc_ent *fc = &cache[i];

		if (fc->valid && fc->hash == hash)
			return fc;
		if (!fc->valid)
			return fc;
		if (time_before(fc->ts, oldest->ts))
			oldest = fc;
	}

	return oldest;
}

/* Refresh the smoothed per-link utilisation from the WTBL airtime counters.
 * Values are microseconds of consumed TX airtime, cumulative, indices 0..3
 * being the four TX ACs. Deltas -- never the raw cumulative value -- drive
 * the weight. Updates race benignly: a stale sample only yields a slightly
 * old weight, so no lock is taken on the TX path.
 */
void mt7925_mlo_sample_load(struct mt792x_dev *dev)
{
	unsigned long now = jiffies;
	int idx;

	/* This shared module also serves MT7921/MT7922. Experimental MLO
	 * load state must only be populated by the MT7927 DUT.
	 */
	if (!is_mt7927(&dev->mt76))
		return;

	for (idx = 0; idx < ARRAY_SIZE(dev->mt76.wcid); idx++) {
		struct mt76_wcid *wcid = rcu_dereference_protected(
			dev->mt76.wcid[idx], lockdep_is_held(&dev->mt76.mutex));
		struct mt7925_mlo_link_load *ld;
		struct mt792x_link_sta *ls;
		struct mt792x_sta *msta;
		struct ieee80211_vif *vif;
		u64 cur = 0, delta;
		u32 elapsed_us, util;
		u8 link_id;
		int ac;

		if (!wcid || !wcid->sta || !wcid->link_valid)
			continue;

		ls = container_of(wcid, struct mt792x_link_sta, wcid);
		msta = ls->sta;
		link_id = wcid->link_id;
		if (!msta || !msta->vif)
			continue;
		vif = container_of((void *)msta->vif, struct ieee80211_vif,
				   drv_priv);
		if (!ieee80211_vif_is_mld(vif) ||
		    !(vif->active_links & BIT(link_id)))
			continue;

		ld = &mt7925_mlo_load[link_id];


		for (ac = 0; ac < 4; ac++)
			cur += ls->airtime_ac[ac];

		if (!ld->last_ts) {          /* first sample: seed only */
			ld->last_tx_us = cur;
			ld->last_ts = now;
			continue;
		}

		/* mt7925_mac_sta_poll() clears all admission counters once one
		 * reaches BIT(30). A backwards step is therefore an explicit
		 * clear; cur is the complete post-clear contribution.
		 */
		delta = mt7925_mlo_airtime_delta(cur, ld->last_tx_us);
		elapsed_us = jiffies_to_usecs(now - ld->last_ts);
		if (!elapsed_us)
			continue;

		util = mt7925_mlo_raw_util(delta, elapsed_us);

		/* EMA, alpha = 1/4: smooths jitter and stops a single spike
		 * from collapsing the weight.
		 */
		WRITE_ONCE(ld->ema_util,
			   mt7925_mlo_ema_util(READ_ONCE(ld->ema_util), util));
		WRITE_ONCE(ld->raw_util, util);
		WRITE_ONCE(ld->last_delta_us, min_t(u64, delta, U32_MAX));
		WRITE_ONCE(ld->elapsed_us, elapsed_us);
		ld->last_tx_us = cur;
		ld->last_ts = now;
	}
}
EXPORT_SYMBOL_GPL(mt7925_mlo_sample_load);

u32 mt7925_mlo_dbg_elapsed(u8 link_id)
{
	return READ_ONCE(mt7925_mlo_load[link_id].elapsed_us);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_dbg_elapsed);

u32 mt7925_mlo_dbg_raw_util(u8 link_id)
{
	return READ_ONCE(mt7925_mlo_load[link_id].raw_util);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_dbg_raw_util);

u32 mt7925_mlo_dbg_util(u8 link_id)
{
	return READ_ONCE(mt7925_mlo_load[link_id].ema_util);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_dbg_util);

u32 mt7925_mlo_dbg_delta(u8 link_id)
{
	return READ_ONCE(mt7925_mlo_load[link_id].last_delta_us);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_dbg_delta);

u32 mt7925_mlo_dbg_spare(u8 link_id)
{
	return READ_ONCE(mt7925_mlo_load[link_id].spare_weight);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_dbg_spare);

u32 mt7925_mlo_dbg_weight(u8 link_id)
{
	return READ_ONCE(mt7925_mlo_load[link_id].final_weight);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_dbg_weight);

u64 mt7925_mlo_dbg_placements(u8 link_id)
{
	return atomic64_read(&mt7925_mlo_placements[link_id]);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_dbg_placements);

u64 mt7925_mlo_dbg_cache_hits(u8 link_id)
{
	return atomic64_read(&mt7925_mlo_cache_hits[link_id]);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_dbg_cache_hits);

static atomic_t mt7925_mlo_alternate_ctr = ATOMIC_INIT(0);

/* Invalidate a STA's TID->link pin(s) -- called when the thing the pin's
 * safety depended on goes away: a BA session ending (tid >= 0, one TID) or
 * the active-link set changing (tid < 0, all TIDs, since a pinned link may
 * no longer be active).
 */
void mt7925_mlo_clear_tid_link(struct mt792x_sta *msta, int tid)
{
	if (tid < 0) {
		memset(msta->mlo_tid_link_valid, 0,
		      sizeof(msta->mlo_tid_link_valid));
		return;
	}
	if (tid < IEEE80211_NUM_TIDS)
		msta->mlo_tid_link_valid[tid] = false;
}
EXPORT_SYMBOL_GPL(mt7925_mlo_clear_tid_link);

/* EXPERIMENTAL: bulk-TXQ counterpart to the mt792x_tx() override above.
 * Invoked through the per-device mt76 TX selection callback so
 * mt76_txq_schedule_list() re-resolves the WCID for this dequeue instead
 * of permanently using whatever was cached on the mac80211 txq at queue
 * creation. Returns NULL (no override) unless mt7925_mlo_force_tx_link is
 * active AND the target link has a live, valid WCID.
 */
/* Restored (2026-08-23): the CLASS3 investigation's own removal note said
 * this was "temporary diagnostic only," but real live testing right after
 * removal exposed the actual bug this was silently covering: the bulk
 * scheduler starts sending on a secondary link ~24ms after its local WCID
 * publish (MLO_LINK_READY) -- i.e. the driver treats the link as TX-ready
 * purely on local firmware state, with no signal that the AP has caught
 * up. Withholding secondary links from mt7925_mlo_tx_select_wcid() for a
 * short window after publish keeps traffic on the primary link (deflink)
 * until then. The primary link itself is never gated. This is explicitly
 * a stopgap: the real fix is to replace this arbitrary delay with a real
 * AP-readiness signal once one is identified.
 */
#define MT7925_MLO_TX_STABILIZE_MS 500

/* How long a TID stays pinned to the link it was first assigned to before
 * being allowed a fresh, quality-aware re-pick. Keeps load balancing
 * dynamic (a link that degrades or a better one that appears gets picked
 * up within one interval) without moving a live BA session mid-stream
 * (see mlo_tid_link_ts comment, mt792x.h) -- each re-pick is a clean new
 * BA session, not a split of the old one.
 */
#define MT7925_MLO_TID_REBALANCE_MS 3000

/* Minimum score margin a candidate must beat the CURRENT link by, at a
 * rebalance-timer expiry, before migrating a TID off it. Without this, a TID
 * would unconditionally re-roll every MT7925_MLO_TID_REBALANCE_MS even when
 * the current link is still fine, causing needless BA churn on tiny RSSI
 * noise. Only a real, material improvement is worth a fresh BA session.
 */
#define MT7925_MLO_TX_SCORE_HYSTERESIS_DB 6

/* Placeholder score for a candidate whose WCID has not yet received a single
 * real RX frame to average an RSSI from. ewma_signal_read() on a freshly
 * ewma_signal_init()'d wcid reads 0 -- as a negated dBm score that is an
 * unrealistically strong value that would otherwise let a never-measured
 * substitute always win the very first placement. Use a fixed, deliberately
 * mediocre placeholder instead: comparable, never automatically dominant.
 */
#define MT7925_MLO_TX_SCORE_NO_DATA (-60)

/* Deterministic best-eligible-link TID placement instead of the legacy
 * first-eligible-in-rotation-order pick. Off by default: this changes which
 * link every new TID lands on, and production behaviour must not change
 * silently before this has been validated live against the legacy policy.
 */
static bool mt7925_mlo_tx_best_link_policy;
module_param_named(mlo_tx_best_link_policy, mt7925_mlo_tx_best_link_policy,
		   bool, 0644);
MODULE_PARM_DESC(mlo_tx_best_link_policy,
		 "Deterministic best-eligible-link TID placement instead of first-eligible round robin (default N)");

/* Default 1: address every unicast data frame of an MT7927 MLD station with the primary active link's WCID. Each WCID
 * owns a separate TX PN counter but the AP replays against one MLD-wide window, so frames addressed to a WCID whose
 * counter lags the other one's are dropped until it catches up. Hardware evidence: pre-registered per-WCID PN gaps
 * (+101/+9/+148/-26/+151/-41) predicted the unanswered-ping prefix 6/6; on the refactored modules the 0 setting collapses (0x6 ping 23/100, iperf zero windows) and 1 sustains
 * 392-781 Mbit/s on 0x3/0x5/0x6. The firmware still distributes that WCID's frames
 * over both radios (TX status band0 and band1 both observed), so this is not a single-radio policy. The setting must be
 * in effect before association: a WCID counter that has already run ahead is not recovered by switching later.
 */
static int mt7925_mlo_tx_primary_handle = 1;
module_param_named(mlo_tx_primary_handle, mt7925_mlo_tx_primary_handle, int, 0644);
MODULE_PARM_DESC(mlo_tx_primary_handle,
		 "MT7927 MLD unicast data handle: 0=per-TID link WCID (collapses on PN ordering), 1=primary active link WCID (default), 2=association-stable deflink WCID even after its link left");

static bool mt7925_mlo_link_tx_stable(struct mt792x_sta *msta, u8 link_id)
{
	unsigned long ready_ts;

	if (link_id == msta->deflink_id)
		return true;

	ready_ts = msta->mlo_link_ready_jiffies[link_id];
	if (!ready_ts)
		return false;

	return time_after(jiffies,
			  ready_ts + msecs_to_jiffies(MT7925_MLO_TX_STABILIZE_MS));
}

/* Real, cheap, already-maintained per-link signal quality: struct mt76_wcid's
 * own ewma_signal average, updated passively from every real RX frame on
 * that WCID -- no active hardware RSSI-monitor arm required (unlike
 * mvif->mlo_link_rssi[], which is proven live to risk disconnecting the AP
 * when armed against a link the driver isn't already actively scheduling
 * on; see mt7925_mlo_rotate_rssi_monitor()'s comment, main.c). S32_MIN means
 * genuinely ineligible (no valid WCID); MT7925_MLO_TX_SCORE_NO_DATA is the
 * deliberate placeholder for "eligible but never yet measured".
 */
static s32 mt7925_mlo_link_tx_score(struct mt792x_sta *msta, u8 link_id)
{
	struct mt792x_link_sta *cand = mt792x_sta_to_link(msta, link_id);
	s32 rssi;

	if (!cand || !cand->wcid.link_valid)
		return S32_MIN;

	rssi = -(s32)ewma_signal_read(&cand->wcid.rssi);
	return rssi ? rssi : MT7925_MLO_TX_SCORE_NO_DATA;
}

/* Bounded recent window for "RX is progressing right now". Deliberately
 * short: this only needs to prove the link is alive *now*, not average out
 * traffic bursts -- a link that hasn't moved an RX packet in 2s while TID
 * traffic is actively flowing on the pair is not a link worth trusting on
 * RSSI alone.
 */
#define MT7925_MLO_TX_LIVENESS_WINDOW_MS	2000
/* Minimum accumulated TX before "never seen RX" is treated as proof of
 * failure (DEAD) rather than "hasn't had a real chance yet" (UNKNOWN).
 * Below this, a link that was JUST activated and genuinely has not had time
 * to prove itself must not be punished as if it had already failed.
 */
#define MT7925_MLO_TX_LIVENESS_DEAD_TX_THRESHOLD	10

/* Classifies link_id's REAL RX delivery, not its signal strength -- proven
 * live (2026-09-12) to be the actual gap: mt7925_mlo_link_tx_score() reports
 * RSSI alone and confidently scored a link at -60dBm while that link's real
 * wcid.stats.rx_packets stayed at 0 for the entire test (tx_pkts=45 in the
 * same window), and the deterministic best-link policy pinned every TID to
 * it, producing 100% ARP/ping failure. Round-robin only avoided this by
 * accident (it also spends time on the healthy link). This has nothing to
 * do with signal quality and everything to do with whether real 802.11 ACKed
 * RX is actually happening -- so it is tracked here from mt76_wcid.stats,
 * completely independent of mvif->mlo_link_rssi[]/the HW RSSI monitor (which
 * this driver deliberately stopped arming on active links this same day,
 * since arming it there was itself proven to corrupt RX -- see
 * mt7925_mlo_rotate_rssi_monitor()'s comment, main.c. That array's timestamp
 * is no longer refreshed for an active link at all, so it cannot be reused
 * here to mean "RX is recent").
 */
static enum mt7925_mlo_tx_liveness
mt7925_mlo_tx_link_liveness(struct mt792x_link_sta *mlink)
{
	struct mt76_wcid *wcid = &mlink->wcid;
	u32 rx = mlink->mlo_valid_rx_packets;
	u32 tx = wcid->stats.tx_packets;
	unsigned long now = jiffies;

	if (rx != mlink->tx_liveness_rx_seen) {
		mlink->tx_liveness_rx_progress_jiffies = now;
		mlink->tx_liveness_rx_ever_seen = true;
	}
	mlink->tx_liveness_rx_seen = rx;
	mlink->tx_liveness_tx_seen = tx;

	if (mlink->tx_liveness_rx_ever_seen) {
		if (time_before_eq(now, mlink->tx_liveness_rx_progress_jiffies +
					msecs_to_jiffies(MT7925_MLO_TX_LIVENESS_WINDOW_MS)))
			return MT7925_TX_LIVENESS_LIVE;
		return MT7925_TX_LIVENESS_CACHED;
	}

	if (tx >= MT7925_MLO_TX_LIVENESS_DEAD_TX_THRESHOLD)
		return MT7925_TX_LIVENESS_DEAD;

	return MT7925_TX_LIVENESS_UNKNOWN;
}

/* Builds a full liveness+RSSI candidate for link_id, or returns false if the
 * link isn't a real eligible candidate at all (no valid WCID / no score).
 * Score reuses mt7925_mlo_link_tx_score() unchanged -- RSSI itself is real
 * and correct; the only thing that was missing is the liveness gate ahead
 * of it.
 */
static bool
mt7925_mlo_link_tx_candidate(struct mt792x_sta *msta, u8 link_id,
			     struct mt7925_tx_liveness_candidate *out)
{
	struct mt792x_link_sta *mlink = mt792x_sta_to_link(msta, link_id);
	s32 score;

	if (!mlink || !mlink->wcid.link_valid)
		return false;

	score = mt7925_mlo_link_tx_score(msta, link_id);
	if (score == S32_MIN)
		return false;

	out->liveness = mt7925_mlo_tx_link_liveness(mlink);
	out->rssi = score;
	out->link_id = link_id;
	return true;
}

/* Hysteresis-aware comparison for the rebalance-timer path: a strict
 * liveness-tier upgrade (e.g. current is DEAD, candidate is LIVE) always
 * wins unconditionally -- a dead/degraded current link must not be
 * defended by RSSI hysteresis. Within the SAME tier, keep the existing
 * "materially better, not merely different" hysteresis so a TID is not
 * unconditionally re-rolled (restarting BA) every rebalance interval when
 * nothing meaningfully changed.
 */
static bool
mt7925_mlo_tx_candidate_better_hysteresis(const struct mt7925_tx_liveness_candidate *cand,
					  const struct mt7925_tx_liveness_candidate *cur)
{
	int rank_cand = mt7925_mlo_tx_liveness_rank(cand->liveness);
	int rank_cur = mt7925_mlo_tx_liveness_rank(cur->liveness);

	if (rank_cand != rank_cur)
		return rank_cand > rank_cur;

	return cand->rssi > cur->rssi + MT7925_MLO_TX_SCORE_HYSTERESIS_DB;
}

/* AP-negotiated TID-to-link map (mac80211's vif->neg_ttlm), if any, further
 * restricts which active links this TID may use for uplink. Per struct
 * ieee80211_neg_ttlm's own doc comment, uplink[tid] == 0 means "mapping for
 * this TID is not included" -- i.e. unrestricted, not "no links allowed".
 * Never widen the result beyond `active`; only ever narrow it.
 */
static unsigned long mt7925_mlo_tid_eligible_links(struct ieee80211_vif *vif,
						   unsigned long active, u8 tid)
{
	if (vif->neg_ttlm.valid && tid < IEEE80211_TTLM_NUM_TIDS &&
	    vif->neg_ttlm.uplink[tid])
		active &= vif->neg_ttlm.uplink[tid];

	return active;
}

struct mt76_wcid *
mt7925_mlo_tx_select_wcid(struct mt76_dev *dev, struct mt76_wcid *wcid,
			    struct ieee80211_sta *sta, struct sk_buff *skb)
{
	struct mt792x_link_sta *forced;
	struct mt792x_sta *msta;
	struct ieee80211_vif *vif;
	int target = mt7925_mlo_force_tx_link;

	if (!is_mt7927(dev) || !sta)
		return NULL;
	msta = (struct mt792x_sta *)sta->drv_priv;
	if (!msta->vif || !is_mt7927(&msta->vif->phy->dev->mt76))
		return NULL;
	vif = container_of((void *)msta->vif, struct ieee80211_vif, drv_priv);
	if (!ieee80211_vif_is_mld(vif))
		return NULL;

	/* EAPOL/control-port frames (4-way handshake, GTK rekey) must stay on
	 * the primary/anchor link: the AP's hostapd only listens for the
	 * handshake on the vap tied to the link it granted the MLO peer's
	 * primary umac on, not on newly-added secondary links. mac80211 marks
	 * these in ieee80211_tx_h_check_control_port_protocol() (tx.c) by
	 * comparing skb->protocol against sdata->control_port_protocol
	 * (ETH_P_PAE), regardless of which TX path (immediate or queued/
	 * dequeue) the frame takes -- so this flag is already set on `skb` by
	 * the time it reaches here. This check runs before, and takes
	 * priority over, mlo_force_tx_link and the TID scheduler below: EAPOL
	 * pinning is unconditional, not part of either steering mode.
	 */
	if (skb) {
		struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);

		if (info->control.flags & IEEE80211_TX_CTRL_PORT_CTRL_PROTO) {
			forced = mt792x_sta_to_link(msta, msta->deflink_id);
			if (!forced || !forced->wcid.link_valid)
				return NULL;

			if (READ_ONCE(mt76_mlo_diag_trace))
				printk(KERN_INFO
				      "MLO_EAPOL_PIN: sta=%pM deflink=%u wcid=%u\n",
				      sta->addr, msta->deflink_id, forced->wcid.idx);

			return &forced->wcid;
		}
	}

	/* Default (no mlo_force_tx_link override active): pin each TID to
	 * one active link for the life of its BA session, instead of either
	 * (a) always deflink (original bug) or (b) re-picking per packet
	 * (tried, broke BA -- see the struct field comment on
	 * msta->mlo_tid_link in mt792x.h). First packet of an unmapped TID
	 * round-robins to choose; every later packet with the same TID
	 * reuses that choice until mt7925_mlo_clear_tid_link() invalidates
	 * it (AMPDU stop, change_sta_links). Priority order per caller
	 * (mt76_mlo_select_wcid(), tx.c): explicit force modes above this
	 * already took priority via the earlier branches; this only runs
	 * when target == -1 (fully off).
	 */
	if (target == -1) {
		struct ieee80211_vif *vif = container_of((void *)msta->vif,
							 struct ieee80211_vif,
							 drv_priv);
		unsigned long active = vif->active_links;
		u8 tid = skb ? skb->priority & 0xf : 0;
		u8 old_link = 0xff;

		if (!ieee80211_vif_is_mld(vif) || hweight16(active) <= 1)
			return NULL;

		if (READ_ONCE(mt7925_mlo_tx_primary_handle) &&
		    is_mt7927(&msta->vif->phy->dev->mt76)) {
			struct mt792x_link_sta *pri;
			unsigned long valid = 0;
			unsigned int id;
			int plink;

			if (READ_ONCE(mt7925_mlo_tx_primary_handle) == 2) {
				/* Diagnostic mode 2 retains the association deflink handle.
				 * This is not proof that firmware shares key/PN state.
				 */
				pri = mt792x_sta_to_link(msta, msta->deflink_id);
				if (pri)
					return &pri->wcid;
			}

			for_each_set_bit(id, &active, IEEE80211_MLD_MAX_NUM_LINKS) {
				pri = mt792x_sta_to_link(msta, id);
				if (pri && pri->wcid.link_valid)
					valid |= BIT(id);
			}
			plink = mt7925_mlo_tx_primary_link(msta->deflink_id, active, valid);
			if (plink < 0)
				return NULL;
			pri = mt792x_sta_to_link(msta, plink);
			if (!pri)
				return NULL;
			return &pri->wcid;
		}

		if (msta->mlo_tid_link_valid[tid]) {
			bool hard_invalid;
			bool timer_expired;

			old_link = target = msta->mlo_tid_link[tid];
			/* Link may have dropped out of active_links since it
			 * was pinned (e.g. steering switched pairs), or --
			 * restored (2026-08-23) -- lost its stabilization
			 * state; reject the stale pin rather than send on it.
			 * This is unconditional regardless of policy: an
			 * ineligible link is never kept out of hysteresis.
			 */
			hard_invalid = !(active & BIT(target)) ||
				       !mt7925_mlo_link_tx_stable(msta, target);

			/* Dynamic rebalancing: a pin is not permanent. Once it
			 * has lived past MT7925_MLO_TID_REBALANCE_MS, re-evaluate
			 * -- this is what makes link choice actually dynamic over
			 * time instead of "whichever link won the first packet,
			 * forever." A fresh pick still means a new BA session
			 * (this TID's old scoreboard is abandoned cleanly, not
			 * split mid-stream), so it does not reintroduce the
			 * sequence-space problem the per-packet approach had.
			 */
			timer_expired = time_after(jiffies,
						   msta->mlo_tid_link_ts[tid] +
						   msecs_to_jiffies(MT7925_MLO_TID_REBALANCE_MS));

			if (hard_invalid) {
				msta->mlo_tid_link_valid[tid] = false;
				target = -1;
			} else if (timer_expired) {
				if (READ_ONCE(mt7925_mlo_tx_best_link_policy)) {
					/* Hysteresis: at a rebalance-timer expiry,
					 * only migrate off an otherwise-still-eligible
					 * link if some other eligible candidate is
					 * MATERIALLY better, not merely different.
					 * Without this, a TID would unconditionally
					 * re-roll (and restart BA) every rebalance
					 * interval even when nothing changed.
					 */
					unsigned long ttlm_eligible =
						mt7925_mlo_tid_eligible_links(vif, active, tid);
					struct mt7925_tx_liveness_candidate cur_cand;
					struct mt7925_tx_liveness_candidate best_cand;
					int best_link = target;
					int link_id;

					if (!mt7925_mlo_link_tx_candidate(msta, target,
									  &cur_cand)) {
						cur_cand.liveness = MT7925_TX_LIVENESS_DEAD;
						cur_cand.rssi = S32_MIN;
						cur_cand.link_id = target;
					}
					best_cand = cur_cand;

					for_each_set_bit(link_id, &ttlm_eligible,
							 IEEE80211_MLD_MAX_NUM_LINKS) {
						struct mt7925_tx_liveness_candidate cand;

						if (link_id == target ||
						    !mt7925_mlo_link_tx_stable(msta, link_id))
							continue;

						if (!mt7925_mlo_link_tx_candidate(msta, link_id,
										  &cand))
							continue;
						if (mt7925_mlo_tx_candidate_better_hysteresis(&cand,
											      &best_cand)) {
							best_cand = cand;
							best_link = link_id;
						}
					}

					if (READ_ONCE(mt76_mlo_diag_trace))
						printk(KERN_INFO
						      "MLO_TX_POLICY: tid=%u active=0x%lx ttlm=0x%lx current=%u current_liveness=%d current_rssi=%ld candidate=%d candidate_liveness=%d candidate_rssi=%ld reason=rebalance_check\n",
						      tid, active, ttlm_eligible, target,
						      cur_cand.liveness, cur_cand.rssi,
						      best_link, best_cand.liveness, best_cand.rssi);

					if (best_link != target) {
						msta->mlo_tid_link_valid[tid] = false;
						target = -1;
					} else {
						/* No materially better candidate:
						 * extend the current pin instead of
						 * unconditionally re-rolling.
						 */
						msta->mlo_tid_link_ts[tid] = jiffies;
					}
				} else {
					/* Legacy policy: unconditional re-pick every
					 * rebalance interval, unchanged behaviour.
					 */
					msta->mlo_tid_link_valid[tid] = false;
					target = -1;
				}
			}
		}

		if (target == -1) {
			unsigned long candidates =
				mt7925_mlo_tid_eligible_links(vif, active, tid);
			int best = -1;

			if (READ_ONCE(mt7925_mlo_tx_best_link_policy)) {
				struct mt7925_tx_liveness_candidate best_cand = {};
				bool have_best = false;
				int link_id;

				/* Deterministic best-eligible-link pick: liveness
				 * class first (a link with zero real RX can never
				 * win just because its RSSI looks good), then real
				 * per-link RSSI within the same liveness class, then
				 * a fixed link_id tiebreak -- never loop/cursor-state
				 * luck. See mlo_tx_liveness.h.
				 */
				for_each_set_bit(link_id, &candidates,
						 IEEE80211_MLD_MAX_NUM_LINKS) {
					struct mt7925_tx_liveness_candidate cand;

					if (!mt7925_mlo_link_tx_stable(msta, link_id))
						continue;

					if (!mt7925_mlo_link_tx_candidate(msta, link_id, &cand))
						continue;
					if (!have_best ||
					    mt7925_mlo_tx_candidate_better(&cand, &best_cand)) {
						best_cand = cand;
						have_best = true;
					}
				}

				best = have_best ? best_cand.link_id : -1;

				if (READ_ONCE(mt76_mlo_diag_trace))
					printk(KERN_INFO
					      "MLO_TX_POLICY: tid=%u active=0x%lx ttlm=0x%lx eligible=0x%lx selected=%d liveness=%d rssi=%ld reason=fresh_pick\n",
					      tid, active,
					      mt7925_mlo_tid_eligible_links(vif, active, tid),
					      candidates, best,
					      have_best ? (int)best_cand.liveness : -1,
					      have_best ? best_cand.rssi : 0L);
			} else {
				u8 next = msta->mlo_tx_link_cursor;
				int tries;

				/* Pair steering has already selected the eligible links by
				 * quality. Distribute independent TIDs across that pair so
				 * one strong member does not starve the other completely;
				 * each selected TID remains pinned until its rebalance point.
				 */
				for (tries = 0; tries < IEEE80211_MLD_MAX_NUM_LINKS; tries++) {
					next = (next + 1) % IEEE80211_MLD_MAX_NUM_LINKS;
					if ((candidates & BIT(next)) &&
					    mt7925_mlo_link_tx_stable(msta, next)) {
						best = next;
						break;
					}
				}
			}

			if (best < 0) {
				if (READ_ONCE(mt76_mlo_diag_trace))
					printk(KERN_INFO
					      "MLO_TX_STABILIZE_WAIT: sta=%pM tid=%u active_links=0x%lx\n",
					      sta->addr, tid, active);
				return NULL;
			}
			msta->mlo_tx_link_cursor = best;
			target = best;
			msta->mlo_tid_link[tid] = target;
			msta->mlo_tid_link_valid[tid] = true;
			msta->mlo_tid_link_ts[tid] = jiffies;
		}

		forced = mt792x_sta_to_link(msta, target);
		if (!forced || !forced->wcid.link_valid) {
			msta->mlo_tid_link_valid[tid] = false;
			return NULL;
		}

		if (READ_ONCE(mt76_mlo_diag_trace)) {
			unsigned long ready_ts = msta->mlo_link_ready_jiffies[target];
			long ready_ms = ready_ts ?
				jiffies_to_msecs(jiffies - ready_ts) : -1;

			printk(KERN_INFO
			      "MLO_TX_TID_SELECT: sta=%pM tid=%u old_link=%u selected_link=%u agg_state=0x%lx\n",
			      sta->addr, tid, old_link, target,
			      forced->wcid.ampdu_state);
			/* link_ready_state: ms since this link's STA_REC was
			 * published (MLO_LINK_READY), or -1 if never published.
			 * This is driver/firmware-local readiness only -- it does
			 * NOT prove the AP has activated the link on its side.
			 */
			printk(KERN_INFO
			      "MLO_TX_SELECT: tid=%u selected_link=%u wcid=%u link_ready_state=%ld\n",
			      tid, target, forced->wcid.idx, ready_ms);
		}

		return &forced->wcid;
	}

	if (target == MT7925_MLO_FORCE_ALTERNATE) {
		/* Only links 0 and 2 have ever had a valid WCID in this
		 * whole investigation; link 1 is deliberately excluded
		 * rather than probed each call.
		 */
		target = (atomic_inc_return(&mt7925_mlo_alternate_ctr) & 1) ? 0 : 2;
	} else if (target == MT7925_MLO_FORCE_FLOWHASH) {
		u32 hash;

		/* Per-flow selection. skb->hash is the 5-tuple flow hash the
		 * network stack computed and cached before 802.11 encap:
		 * ieee80211_select_queue() calls skb_get_hash() while skb->data
		 * still points at the ethhdr (net/mac80211/wme.c). Reading it
		 * here returns that cached value; it is never recomputed over
		 * the 802.11 header.
		 *
		 * Same flow -> same hash -> same link, so a single flow never
		 * splits and cannot reorder against itself. Distinct flows hash
		 * independently and spread across the valid links.
		 *
		 * If the hash was never established, decline rather than guess.
		 */
		if (!skb || !(skb->l4_hash || skb->sw_hash))
			return NULL;

		hash = skb->hash;
		target = (hash & 1) ? 2 : 0;
	} else if (target == MT7925_MLO_FORCE_CAPHASH ||
		   target == MT7925_MLO_FORCE_LOADAWARE) {
		u32 base[IEEE80211_MLD_MAX_NUM_LINKS] = {};
		u32 spare[IEEE80211_MLD_MAX_NUM_LINKS] = {};
		u32 w[IEEE80211_MLD_MAX_NUM_LINKS] = {};
		bool load_aware = (target == MT7925_MLO_FORCE_LOADAWARE);
		u32 bucket, acc = 0;
		struct mt7925_mlo_fc_ent *fc;
		bool unknown = false;
		unsigned long now;
		int i;

		if (!skb || !(skb->l4_hash || skb->sw_hash))
			return NULL;

		now = jiffies;

		/* Existing flow? Keep it where it is -- never re-place a live
		 * flow, regardless of how the weights have moved.
		 */
		if (load_aware) {
			spin_lock_bh(&mt7925_mlo_fc_lock);
			fc = mt7925_mlo_fc_lookup(mt7925_mlo_fc, skb->hash);
			if (fc->valid && fc->hash == skb->hash &&
			    time_before(now, fc->ts +
					msecs_to_jiffies(MT7925_MLO_FC_AGE_MS))) {
				forced = mt792x_sta_to_link(msta, fc->link);
				if (forced && forced->wcid.link_valid) {
					fc->ts = now;
					target = fc->link;
					atomic64_inc(&mt7925_mlo_cache_hits[target]);
					spin_unlock_bh(&mt7925_mlo_fc_lock);
					goto resolve;
				}
				fc->valid = false;
			}
			spin_unlock_bh(&mt7925_mlo_fc_lock);
		}

		/* Weight each valid link by its own operating bandwidth.
		 *
		 * wcid->rate is NOT usable here: it is written from the TXS
		 * report, and an idle link's TXS carries the other link's PHY
		 * parameters (a 20 MHz link is observed reporting bw=320MHz
		 * once it stops transmitting). That makes the weights swing
		 * between ~1:1 and ~20:1 and remaps every flow.
		 *
		 * The link's chan_def is authoritative per link and is driver
		 * /mac80211 configuration state, not firmware telemetry: it is
		 * set when the chanctx is assigned (mt7925/main.c) and stays
		 * valid for as long as the link exists, whether or not that
		 * link is currently transmitting. So it cannot converge on the
		 * other link's value while idle, which is what the scheduler
		 * needs for a stable flow->link mapping.
		 *
		 * This is a configured-bandwidth weight, not a measured
		 * throughput: it ranks the links correctly and never
		 * oscillates. Load-awareness is deliberately left out for now.
		 */
		for (i = 0; i < IEEE80211_MLD_MAX_NUM_LINKS; i++) {
			struct mt792x_link_sta *ls = mt792x_sta_to_link(msta, i);
			struct mt792x_bss_conf *mconf;

			if (!ls || !ls->wcid.link_valid)
				continue;

			mconf = mt792x_vif_to_link(msta->vif, i);
			if (!mconf || !mconf->mt76.ctx) {
				unknown = true;
				continue;
			}

			base[i] = cfg80211_chandef_get_width(&mconf->mt76.ctx->def);
			if (!base[i])
				unknown = true;
			spare[i] = load_aware ?
				mt7925_mlo_spare_weight(base[i],
					READ_ONCE(mt7925_mlo_load[i].ema_util)) :
				base[i];
		}
		mt7925_mlo_normalize_weights(spare, w,
					     IEEE80211_MLD_MAX_NUM_LINKS,
					     MT7925_MLO_BUCKETS);
		if (load_aware) {
			for (i = 0; i < IEEE80211_MLD_MAX_NUM_LINKS; i++) {
				WRITE_ONCE(mt7925_mlo_load[i].spare_weight,
					   spare[i]);
				WRITE_ONCE(mt7925_mlo_load[i].final_weight, w[i]);
			}
		}

		/* If any link's width could not be resolved, fall back to the
		 * plain flow hash rather than skewing the split on partial
		 * information.
		 */
		if (unknown)
			return NULL;

		/* Proportional bucket assignment. Hashing into a fixed bucket
		 * space and handing each link a share of buckets keeps a flow
		 * on one link: when the measured rates shift, only buckets at
		 * a boundary move, so established flows are not all remapped.
		 */
		bucket = (skb->hash >> 8) % MT7925_MLO_BUCKETS;
		target = -1;
		for (i = 0; i < IEEE80211_MLD_MAX_NUM_LINKS; i++) {
			if (!w[i])
				continue;
			acc += w[i];
			if (bucket < acc) {
				target = i;
				break;
			}
		}
		if (target < 0)
			return NULL;

		/* Remember this new mode-6 flow's affinity placement. */
		if (load_aware) {
			spin_lock_bh(&mt7925_mlo_fc_lock);
			fc = mt7925_mlo_fc_lookup(mt7925_mlo_fc, skb->hash);
			fc->hash = skb->hash;
			fc->link = (u8)target;
			fc->ts = now;
			fc->valid = true;
			atomic64_inc(&mt7925_mlo_placements[target]);
			spin_unlock_bh(&mt7925_mlo_fc_lock);
		}
	}

resolve:

	forced = mt792x_sta_to_link(msta, target);
	if (!forced || !forced->wcid.link_valid)
		return NULL;

	if (&forced->wcid == wcid)
		return NULL; /* already on the requested link, no-op */

	return &forced->wcid;
}


EXPORT_SYMBOL_GPL(mt7925_mlo_tx_select_wcid);

static struct mt792x_link_sta *
mt7925_mlo_requested_link(struct ieee80211_vif *vif,
			   struct mt792x_sta *msta, u8 link_id)
{
	struct mt792x_link_sta *mlink;

	/* MT7921/MT7922 vif BMC stations need not have a back-pointer. */
	if (!msta)
		return NULL;
	if (!msta->vif)
		return &msta->deflink;

	if (!ieee80211_vif_is_mld(vif))
		return &msta->deflink;

	if (link_id < IEEE80211_LINK_UNSPECIFIED)
		return rcu_dereference(msta->link[link_id]);

	/* link_id == IEEE80211_LINK_UNSPECIFIED: mac80211 leaves normal MLD
	 * unicast TX unassigned (net/mac80211/tx.c, "address from the MLD"
	 * case) rather than picking a link itself -- previously this always
	 * fell through to msta->deflink. Round-robin across vif->active_links
	 * instead, so traffic actually distributes across active links. The
	 * mlo_force_tx_link debugfs override (mt792x_tx(), mt792x_core.c)
	 * runs after this and takes priority when set.
	 */
	{
		unsigned long active = vif->active_links;
		u8 next = msta->mlo_tx_link_cursor;
		int tries;

		if (hweight16(active) <= 1) {
			mlink = &msta->deflink;
			goto trace;
		}

		for (tries = 0; tries < IEEE80211_MLD_MAX_NUM_LINKS; tries++) {
			next = (next + 1) % IEEE80211_MLD_MAX_NUM_LINKS;
			if (active & BIT(next))
				break;
		}
		msta->mlo_tx_link_cursor = next;

		mlink = rcu_dereference(msta->link[next]);
		if (!mlink)
			mlink = &msta->deflink;

trace:
		if (READ_ONCE(mt76_mlo_diag_trace))
			printk(KERN_INFO
			      "MLO_TX_SELECT: requested=unspecified active_links=0x%lx selected_link=%d\n",
			      active, mlink->wcid.link_id);
	}

	return mlink;
}

static struct mt792x_link_sta *
mt7925_mlo_immediate_link(struct ieee80211_vif *vif, struct mt792x_sta *sta,
		   u8 link_id)
{
	struct mt792x_link_sta *mlink;
	unsigned long active;
	unsigned int id;

	if (!sta->vif)
		return &sta->deflink;

	if (!vif || !ieee80211_vif_is_mld(vif))
		return mt7925_mlo_requested_link(vif, sta, link_id);

	active = vif->active_links;
	mlink = mt7925_mlo_requested_link(vif, sta, link_id);
	if ((active & BIT(link_id)) && mlink && mlink->wcid.link_valid)
		return mlink;

	for_each_set_bit(id, &active, IEEE80211_MLD_MAX_NUM_LINKS) {
		mlink = mt7925_mlo_requested_link(vif, sta, id);
		if (mlink && mlink->wcid.link_valid)
			return mlink;
	}

	return NULL;
}

/* The shared fast paths dispatch through per-device callbacks; no global
 * hook or reverse dependency from mt792x into the MLO owner is required.
 */
void mt7925_mlo_tx_update_load(struct mt76_dev *dev)
{
	mt7925_mlo_sample_load(container_of(dev, struct mt792x_dev, mt76));
}
EXPORT_SYMBOL_GPL(mt7925_mlo_tx_update_load);

struct mt76_wcid *
mt7925_mlo_tx_select_link(struct mt76_dev *dev, struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta, struct sk_buff *skb,
			  struct mt76_wcid *original_wcid)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct mt792x_link_sta *mlink;
	struct mt792x_sta *msta;
	u8 requested;

	if (!is_mt7927(dev) || !vif || !ieee80211_vif_is_mld(vif))
		return original_wcid;
	msta = sta ? (struct mt792x_sta *)sta->drv_priv :
		&((struct mt792x_vif *)vif->drv_priv)->sta;
	if (!msta->vif)
		return original_wcid;

	requested = u32_get_bits(info->control.flags, IEEE80211_TX_CTRL_MLO_LINK);
	mlink = mt7925_mlo_immediate_link(vif, msta, requested);
	if (!mlink || !sta)
		return mlink ? &mlink->wcid : NULL;

	if (READ_ONCE(mt7925_mlo_force_tx_link) >= 0) {
		struct mt792x_link_sta *forced;

		forced = mt792x_sta_to_link(msta, READ_ONCE(mt7925_mlo_force_tx_link));
		if (forced && forced->wcid.link_valid)
			requested = READ_ONCE(mt7925_mlo_force_tx_link);
	}
	/* Preserve the two original shared immediate lookups/cursor effects.
	 * This is separate from the queued per-TID/BA selector above.
	 */
	mlink = mt7925_mlo_immediate_link(vif, msta, requested);
	return mlink ? &mlink->wcid : NULL;
}
EXPORT_SYMBOL_GPL(mt7925_mlo_tx_select_link);

/* DUT-only native DATA preservation policy. Generic header translation and
 * skb/PM submission remain shared dependencies. Default OFF.
 */

static bool mt7925_mlo_preserve_data_da;
module_param_named(mlo_preserve_data_da, mt7925_mlo_preserve_data_da, bool, 0644);
MODULE_PARM_DESC(mlo_preserve_data_da,
                 "EXPERIMENTAL: preserve native MT7927 MLO ToDS destination in DUT-only TX (default N)");

bool mt7925_mlo_tx_preserve_addr3(struct mt76_dev *dev,
				struct ieee80211_vif *vif, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (void *)skb->data;

	return READ_ONCE(mt7925_mlo_preserve_data_da) && is_mt7927(dev) &&
		vif && vif->type == NL80211_IFTYPE_STATION &&
		ieee80211_vif_is_mld(vif) &&
		skb->len >= sizeof(struct ieee80211_hdr_3addr) &&
		ieee80211_is_data(hdr->frame_control) &&
		ieee80211_has_tods(hdr->frame_control);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_tx_preserve_addr3);

/* TEST-ONLY BA diagnostic gates; both preserve normal behavior by default. */
static bool mt7925_test_no_tx_ba;
module_param_named(test_mt7927_no_tx_ba, mt7925_test_no_tx_ba, bool, 0644);
MODULE_PARM_DESC(test_mt7927_no_tx_ba,
		 "TEST-ONLY: refuse TX Block-Ack session setup (default N)");

static bool mt7925_test_no_rx_ba;
module_param_named(test_mt7927_no_rx_ba, mt7925_test_no_rx_ba, bool, 0644);
MODULE_PARM_DESC(test_mt7927_no_rx_ba,
		 "TEST-ONLY: refuse RX Block-Ack session setup (default N)");
static const char *mt7925_ampdu_action_name(enum ieee80211_ampdu_mlme_action action)
{
	switch (action) {
	case IEEE80211_AMPDU_RX_START: return "RX_START";
	case IEEE80211_AMPDU_RX_STOP: return "RX_STOP";
	case IEEE80211_AMPDU_TX_START: return "TX_START";
	case IEEE80211_AMPDU_TX_OPERATIONAL: return "TX_OPERATIONAL";
	case IEEE80211_AMPDU_TX_STOP_FLUSH: return "TX_STOP_FLUSH";
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT: return "TX_STOP_FLUSH_CONT";
	case IEEE80211_AMPDU_TX_STOP_CONT: return "TX_STOP_CONT";
	default: return "UNKNOWN";
	}
}

int
mt7925_mlo_tx_ampdu_action(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		    struct ieee80211_ampdu_params *params)
{
	enum ieee80211_ampdu_mlme_action action = params->action;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct ieee80211_sta *sta = params->sta;
	struct ieee80211_txq *txq = sta->txq[params->tid];
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	u16 tid = params->tid;
	struct mt76_txq *mtxq;
	int ret = 0;

	if (!txq)
		return -EINVAL;

	if (READ_ONCE(mt7925_test_no_tx_ba) &&
	    action == IEEE80211_AMPDU_TX_START)
		return -EOPNOTSUPP;
	if (READ_ONCE(mt7925_test_no_rx_ba) &&
	    action == IEEE80211_AMPDU_RX_START)
		return -EOPNOTSUPP;

	mtxq = (struct mt76_txq *)txq->drv_priv;

	mt792x_mutex_acquire(dev);
	switch (action) {
	case IEEE80211_AMPDU_RX_START:
	case IEEE80211_AMPDU_RX_STOP:
	case IEEE80211_AMPDU_TX_START:
	case IEEE80211_AMPDU_TX_OPERATIONAL:
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
	case IEEE80211_AMPDU_TX_STOP_CONT: {
		/* Per-link AMPDU bookkeeping: params carries no link_id
		 * (struct ieee80211_ampdu_params has none -- confirmed,
		 * mac80211.h), so apply the state change to every active
		 * link's own wcid, the same active-link iteration
		 * mt7925_mcu_uni_tx_ba() already uses for the real firmware
		 * BA command. Without this, a TID pinned onto a non-deflink
		 * link (mlo_tid_link) transmits with wcid->ampdu_state still
		 * 0 on that link -- firmware never told the link is
		 * aggregating -- even though mac80211 believes AMPDU is
		 * active for the TID.
		 *
		 * mtxq->aggr/send_bar are deliberately left as single,
		 * non-per-link flags: struct mt76_txq is one object per
		 * (sta, tid), not per link, so there is nothing to iterate
		 * there -- it reflects "is this TID aggregating at all",
		 * which is still correct as a single flag.
		 */
		struct ieee80211_link_sta *link_sta;
		unsigned int link_id;

		for_each_sta_active_link(vif, sta, link_sta, link_id) {
			struct mt792x_link_sta *mlink;

			mlink = mt792x_sta_to_link(msta, link_id);
			if (!mlink)
				continue;

			mt7925_mac_ampdu_link(dev, &mlink->wcid, params);

			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_AMPDU_LINK: action=%s tid=%u link=%u wcid=%u state=0x%lx\n",
				      mt7925_ampdu_action_name(action), tid,
				      link_id, mlink->wcid.idx,
				      mlink->wcid.ampdu_state);
		}

		ret = mt7925_mac_ampdu_action(dev, vif, params, mtxq);
		switch (action) {
		case IEEE80211_AMPDU_TX_STOP_FLUSH:
		case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		case IEEE80211_AMPDU_TX_STOP_CONT:
			mt7925_mlo_clear_tid_link(msta, tid);
			if (action == IEEE80211_AMPDU_TX_STOP_CONT)
				ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
			break;
		default:
			break;
		}
		break;
	}
	}
	mt792x_mutex_release(dev);

	return ret;
}
