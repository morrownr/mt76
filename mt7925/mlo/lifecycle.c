// SPDX-License-Identifier: ISC
/*
 * MT7927-specific MLO active-link lifecycle.
 *
 * MT7927 station MLO steering must not treat every active-link removal as a
 * negotiated-link teardown. Keep per-link BSS/WCID identities resident across
 * active pair changes and toggle only traffic eligibility plus the current
 * firmware active-pair mapping.
 */

#include <linux/moduleparam.h>
#include <linux/delay.h>
#include "../mlo_wcid_handover.h"
#include "../mlo_bss_alloc.h"
#include "../mlo_peer_move.h"
#include "../mt7925.h"
#include "radio.h"
#include "pairs.h"
#include "../../trace.h"
#include "../mlo_survivor_refresh.h"
#include "../mlo_pair_transition.h"

/* TEST-ONLY: retry a genuinely-new MLO link's JOIN ROC exactly once after a
 * real firmware -ETIMEDOUT, before the caller tears the freshly-created
 * BSS/WCID state back down. Proven live: mt7925_set_mlo_roc() (main.c)
 * fully clears MT76_STATE_ROC and phy->mlo_roc.held_links via
 * mt7925_abort_mlo_roc_all() on its own timeout path before returning, so a
 * second call with the same arguments starts from a genuinely idle ROC
 * state -- not a retry into busy/leaked state. Default OFF; MT7927 MLO
 * station links only; does not touch dbdcband policy, the resident-link
 * fallback, or band_idx.
 */
/* EXPERIMENT (default N, needs test_mt7927_fw_quiesce_removed): release a retired link's BSS index in host bookkeeping
 * so the next link added reuses it. Hypothesis under test: firmware attaches a STA_REC_MLD member only when its BSS
 * index is below the two-link MLD table size.
 */
static bool mt7925_mlo_free_retired_bss_idx;
module_param_named(mlo_free_retired_bss_idx, mt7925_mlo_free_retired_bss_idx, bool, 0644);
MODULE_PARM_DESC(mlo_free_retired_bss_idx,
		 "EXPERIMENTAL: free a retired MT7927 MLD link's BSS index for reuse (needs test_mt7927_fw_quiesce_removed; default N)");

static bool mt7925_test_mt7927_fw_quiesce_dev;
module_param_named(test_mt7927_fw_quiesce_dev,
		   mt7925_test_mt7927_fw_quiesce_dev, bool, 0644);
MODULE_PARM_DESC(test_mt7927_fw_quiesce_dev,
		 "TEST-ONLY: also disable a removed link's DEV_INFO (default N)");

static bool mt7925_test_mt7927_fw_quiesce_removed;
module_param_named(test_mt7927_fw_quiesce_removed,
		   mt7925_test_mt7927_fw_quiesce_removed, bool, 0644);
MODULE_PARM_DESC(test_mt7927_fw_quiesce_removed,
		 "TEST-ONLY: disable a removed link's firmware STA_REC and BSS_INFO (default N)");

/* EXPERIMENTAL (default N, A/B): after the link-activation JOIN moves a resident
 * link to a different band slot, restore the resident peer's wcid.link_valid
 * that the move parked at false. Without it the anchor's live, keyed peer stays
 * invalid and STA_REC_MLD omits it from the MLD link list.
 */
/* Retain the old parameter for command-line compatibility. Destructive peer
 * refresh now always requires an acknowledged DEV/BSS move; a zero-initialized
 * phy_idx is not evidence that firmware moved the peer.
 */
static bool mt7925_mlo_skip_unmoved_rebuild;
module_param_named(mlo_skip_unmoved_rebuild, mt7925_mlo_skip_unmoved_rebuild,
		   bool, 0644);
MODULE_PARM_DESC(mlo_skip_unmoved_rebuild,
		 "Compatibility parameter: unchanged DEV/BSS peers are always preserved (default N)");

/* EXPERIMENTAL (default N, A/B): keep each MLD peer's wcid.phy_idx equal to its
 * link's band slot. The RX band cross-check in mt7925_mac_fill_rx() compares
 * the descriptor BAND_IDX with every sibling peer's wcid.phy_idx; peers are
 * created with phy_idx 0, so on a stale pair (two peers at 0) a frame received
 * on band 1 matches no candidate and keeps the firmware's WLAN_IDX, which is
 * the wrong link's WCID. Without the destructive recreate (which also rewrites
 * phy_idx) nothing else repairs it.
 */
bool mt7925_mlo_sync_peer_phy;
module_param_named(mlo_sync_peer_phy, mt7925_mlo_sync_peer_phy, bool, 0644);
MODULE_PARM_DESC(mlo_sync_peer_phy,
		 "EXPERIMENTAL: keep MLD peer wcid.phy_idx equal to the link band slot (default N)");

static bool mt7925_mlo_restore_peer_valid;
module_param_named(mlo_restore_peer_valid, mt7925_mlo_restore_peer_valid,
		   bool, 0644);
MODULE_PARM_DESC(mlo_restore_peer_valid,
		 "EXPERIMENTAL: restore resident peer link_valid after the activation JOIN band move (default N)");

static bool mt7925_test_mt7927_migrate_rebuild;
module_param_named(test_mt7927_migrate_rebuild,
		   mt7925_test_mt7927_migrate_rebuild, bool, 0644);
MODULE_PARM_DESC(test_mt7927_migrate_rebuild,
		 "TEST-ONLY: migrate a peer by remove/create/authorize/PTK instead of in-place newly=1 recreate (default N)");

bool mt7925_test_mt7927_ptk_after_mld;
module_param_named(test_mt7927_ptk_after_mld,
		   mt7925_test_mt7927_ptk_after_mld, bool, 0644);
MODULE_PARM_DESC(test_mt7927_ptk_after_mld,
		 "TEST-ONLY: install a freshly admitted link's PTK only after its STA_REC_MLD authorize (default N)");

static bool mt7925_test_mt7927_mlo_join_retry;
module_param_named(test_mt7927_mlo_join_retry,
		   mt7925_test_mt7927_mlo_join_retry, bool, 0644);
MODULE_PARM_DESC(test_mt7927_mlo_join_retry,
		 "TEST-ONLY: retry a newly-created MT7927 MLO link's JOIN ROC once after a real timeout");

/* TEST-ONLY: single-trial probe of the never-wired MT7925_ROC_REQ_SUB_LINK
 * reqtype for a genuinely-new link joining an already-active anchor.
 * Default OFF; MT7927 only; fires at most once per add-loop iteration
 * (one link at a time), with a bounded 1s wait, in place of the full
 * 2-link JOIN. No prior artifact has ever exercised this reqtype -- this
 * is the first live test of whether firmware grants it at all.
 */
/* TEST-ONLY: after a selective release, attach the ADDED link's slot to the
 * still-held JOIN token with a single SUB_LINK-shaped TLV (same token,
 * reqtype JOIN, dbdcband as in the JOIN's second slot) instead of issuing a
 * replacement JOIN under a new token. Default OFF; MT7927 only.
 */
bool mt7925_test_mt7927_mlo_join3;
module_param_named(test_mt7927_mlo_join3, mt7925_test_mt7927_mlo_join3, bool, 0644);
MODULE_PARM_DESC(test_mt7927_mlo_join3,
		 "TEST-ONLY MT7927: the association-time MLO JOIN reserves all three valid links (3 TLVs); default N");

bool mt7925_test_mt7927_mlo_roc_attach;
module_param_named(test_mt7927_mlo_roc_attach, mt7925_test_mt7927_mlo_roc_attach,
		   bool, 0644);
MODULE_PARM_DESC(test_mt7927_mlo_roc_attach,
		 "TEST-ONLY: attach the added link's slot to the held MLO ROC token instead of a replacement JOIN");

/* TEST-ONLY: instead of the production delta path's fresh-token one-TLV
 * ACQUIRE for the replacement link alone (which firmware has repeatedly
 * left ungranted while the continuity owner's token stays held), reuse the
 * continuity owner's HELD token in a two-TLV JOIN: TLV0 re-affirms the
 * survivor's own slot exactly as currently granted, TLV1 requests the
 * replacement. No ROC_ABORT of the survivor is ever sent for this attempt.
 * Default OFF; MT7927 only.
 */
bool mt7925_mlo_same_token_extend;
module_param_named(mlo_same_token_extend, mt7925_mlo_same_token_extend,
		   bool, 0644);
MODULE_PARM_DESC(mlo_same_token_extend,
		 "TEST-ONLY: reuse the continuity owner's held token in a 2-TLV JOIN to request the replacement link");

bool mt7925_test_mt7927_sublink_roc;
module_param_named(test_mt7927_sublink_roc, mt7925_test_mt7927_sublink_roc,
		   bool, 0644);
MODULE_PARM_DESC(test_mt7927_sublink_roc,
		 "TEST-ONLY: probe MT7925_ROC_REQ_SUB_LINK for a new link joining an active anchor");

/* TEST-ONLY: rebind the substitute's BSS/BMC group keys (GTK/IGTK) via
 * mt7925_mlo_group_key_rebind() BEFORE issuing the standalone SUB_LINK ROC
 * above, instead of waiting for mac80211's own later
 * ieee80211_key_switch_links()-driven .set_key() call. Isolates exactly one
 * new variable on top of test_mt7927_sublink_roc: whether firmware requires
 * the substitute's BSS to already be keyed at ROC-request time. Requires
 * test_mt7927_sublink_roc=1 too -- never runs alone, never combined with
 * the full-pair path. Creates no peer STA_REC and never touches
 * wcid.link_valid; only the BSS/BMC-side object mt7927_mlo_add_bss_link()
 * already created is touched.
 */
static bool mt7925_test_mt7927_sublink_prekey;
module_param_named(test_mt7927_sublink_prekey, mt7925_test_mt7927_sublink_prekey,
		   bool, 0644);
MODULE_PARM_DESC(test_mt7927_sublink_prekey,
		 "TEST-ONLY: rebind the substitute's BSS group keys before the standalone SUB_LINK ROC (requires test_mt7927_sublink_roc=1)");

/* TEST-ONLY: defer the standalone SUB_LINK ROC entirely out of the VIF
 * completion phase, to AFTER mac80211's own ieee80211_key_switch_links()
 * has actually enabled the substitute's group key -- proven by source
 * trace to run strictly between the driver's two per-transition
 * change_sta_links() callbacks, and NOT exposed/exported for a driver to
 * trigger early (mt7925_test_mt7927_sublink_prekey's ieee80211_iter_keys()
 * call saw keys_seen=0 live: the key object exists but mac80211 filters
 * out anything not yet KEY_FLAG_UPLOADED_TO_HARDWARE). The VIF phase
 * records pending state and returns; mt7927_mlo_change_sta_links() is the
 * only place that later observes real key readiness and can safely issue
 * ROC. Requires test_mt7927_sublink_roc=1 too; mutually exclusive in
 * practice with test_mt7927_sublink_prekey (this experiment answers "does
 * deferring to the right moment help", not "does forcing it early help").
 */
bool mt7925_test_mt7927_sublink_postkey;
module_param_named(test_mt7927_sublink_postkey, mt7925_test_mt7927_sublink_postkey,
		   bool, 0644);
MODULE_PARM_DESC(test_mt7927_sublink_postkey,
		 "TEST-ONLY: defer the standalone SUB_LINK ROC until after mac80211's key_switch_links has enabled the substitute's group key (requires test_mt7927_sublink_roc=1)");

/* TEST-ONLY: suppress a genuinely-new link's ROC timeout instead of tearing
 * its freshly-created BSS/DEV/WCID state back down, to directly measure
 * whether STA_REC_UPDATE + BSS_INFO(active=1) + band_idx alone are
 * sufficient for firmware to grant real airtime with no ROC grant at all.
 * Both known ROC primitives (JOIN, SUB_LINK) are separately proven to
 * never grant post-association -- this does not assume either works, it
 * measures what happens if we proceed anyway. Default OFF; MT7927 only;
 * does not touch the resident-link (!created&add) tolerance already in
 * place, only extends the same *suppress-and-proceed* shape to the
 * genuinely-new-link case for this one measurement.
 */
/* TEST-ONLY single-link ACQUIRE discriminator: in the survivor-grow phase,
 * instead of the ordinary two-link replacement JOIN, send ONE fresh-token
 * one-TLV ACQUIRE/JOIN for the added link only while the survivor's JOIN
 * slot stays held, observe the firmware for >= 2 s, abort only the probe's
 * own token, then fail the transition through the ordinary error path
 * (never faking a grant, never activating the link, never sending the
 * ordinary replacement JOIN).
 */
static bool mt7925_test_mt7927_roc_single_acquire;
module_param_named(test_mt7927_mlo_roc_single_acquire,
		   mt7925_test_mt7927_roc_single_acquire, bool, 0644);
MODULE_PARM_DESC(test_mt7927_mlo_roc_single_acquire,
		 "TEST-ONLY: single-link fresh-token ACQUIRE probe instead of the replacement JOIN (default N)");

/* TEST-ONLY (needs test_mt7927_mlo_noroc_admit=1 too): when a controlled
 * MLO transition's replacement JOIN was not granted (roc_admitted=0), let the
 * added link continue through the EXISTING normal peer admission lifecycle in
 * mt7927_mlo_change_sta_links() (sta_add_links -> link_valid -> PTK reconcile
 * -> link_sta_assoc/STATE_ASSOC+MLD -> RX-ready wait) instead of the
 * keep_dormant skip. No ROC grant is faked; roc_admitted stays false.
 */
static bool mt7925_test_mt7927_noroc_peer_admit;
module_param_named(test_mt7927_mlo_noroc_peer_admit,
		   mt7925_test_mt7927_noroc_peer_admit, bool, 0644);
MODULE_PARM_DESC(test_mt7927_mlo_noroc_peer_admit,
		 "TEST-ONLY: with noroc_admit, run the normal peer admission for an ungranted added link (default N)");

static bool mt7925_test_mt7927_mlo_noroc_admit;
module_param_named(test_mt7927_mlo_noroc_admit, mt7925_test_mt7927_mlo_noroc_admit,
		   bool, 0644);
MODULE_PARM_DESC(test_mt7927_mlo_noroc_admit,
		 "TEST-ONLY: admit a new MT7927 MLO link on ROC timeout to measure real airtime without a grant");

/* TEST-ONLY: pre-instantiate a standing, quiescent (DEV_INFO + WCID slot
 * only -- no traffic queues, no keys, no ROC) BSS context for every
 * negotiated-but-inactive MLO link at association time, purely so
 * mconf->mt76.idx is real for that link.
 *
 * Root cause proven at source level: mt792x_vif_to_link() (mt792x.h) has
 * no fallback path when mvif->link_conf[link_id] is NULL for an inactive
 * link -- it silently returns &mvif->bss_conf, the PRIMARY link's own
 * context. So mt7925_mlo_rotate_rssi_monitor() -> mt792x_vif_to_bss_conf()
 * -> mt792x_link_conf_to_mconf() for a never-admitted link was arming
 * mt7925_mcu_set_mlo_rssi_monitor() under the ANCHOR's bss_idx the whole
 * time, not the inactive link's -- explaining why the RSSI-monitor
 * rotation never produced real samples for it.
 *
 * Default OFF; MT7927 only. Deliberately does NOT set mvif->valid_links
 * or wcid.link_valid -- every TX/key-iteration site in this driver gates
 * on those, so this link stays fully inert (no traffic, no admission)
 * until it is later admitted for real, at which point
 * mt7927_mlo_add_bss_link() finds these same resident mconf/mlink structs
 * via mt7927_mlo_resident_bss()/_sta() and reuses them instead of
 * allocating new ones -- the resident-link reuse path this file already
 * relies on for active-pair transitions.
 *
 * PROVEN UNSAFE for its original purpose (do not wire this back into RSSI
 * monitoring): arming the hardware RSSI monitor against this dormant
 * link's real bss_idx disconnects the AP within ~1s, even with the two
 * genuinely active links otherwise healthy. MT7927 is a 2-path DBDC radio
 * (see mt7925_mcu_set_mlo_roc()'s fixed 2-element roc[] array) -- sampling
 * a 3rd band's RSSI while 2 bands actively carry traffic appears to steal
 * baseband resources from the real links long enough to look like beacon
 * loss to the AP. mt7925_mlo_rotate_rssi_monitor() and
 * mt7925_mlo_pair_score_parts() (main.c) were both reverted to only ever
 * consider mvif->valid_links (real, active links) after this was found
 * live. This BSS-priming primitive itself is otherwise inert and harmless
 * (no keys, no traffic, no ROC) -- kept only as a documented dead end.
 */
static bool mt7925_test_mt7927_dormant_rssi_bss;
module_param_named(test_mt7927_dormant_rssi_bss,
		   mt7925_test_mt7927_dormant_rssi_bss, bool, 0644);
MODULE_PARM_DESC(test_mt7927_dormant_rssi_bss,
		 "TEST-ONLY: pre-instantiate a dormant BSS context for inactive MT7927 MLO links so RSSI monitoring can reach them");

static bool
mt7927_mlo_active_transition(struct mt792x_dev *dev,
			     struct ieee80211_vif *vif,
			     u16 old_links, u16 new_links)
{
	return old_links && new_links &&
	       is_mt7927(&dev->mt76) &&
	       ieee80211_vif_is_mld(vif) &&
	       vif->type == NL80211_IFTYPE_STATION;
}

static bool
mt7927_mlo_join_retry_allowed(struct mt792x_dev *dev, struct ieee80211_vif *vif)
{
	return READ_ONCE(mt7925_test_mt7927_mlo_join_retry) &&
	       is_mt7927(&dev->mt76) &&
	       ieee80211_vif_is_mld(vif) &&
	       vif->type == NL80211_IFTYPE_STATION;
}

static void
mt7927_mlo_note_bss(struct mt792x_vif *mvif, struct mt792x_bss_conf *mconf,
		    struct mt792x_link_sta *mlink, bool active)
{
	struct mt7927_mlo_state *mlo = &mvif->mt7927_mlo;
	struct mt7927_mlo_link_state *state = &mlo->link[mconf->link_id];

	state->negotiated = true;
	state->resident = true;
	state->traffic_active = active;
	state->link_id = mconf->link_id;
	state->bss_idx = mconf->mt76.idx;
	state->fw_context_slot = mconf->mt76.band_idx;
	state->omac_idx = mconf->mt76.omac_idx;
	state->wmm_idx = mconf->mt76.wmm_idx;
	state->bmc_wcid = mlink->wcid.idx;
	mlink->wcid.link_valid = active;

	mlo->negotiated_links |= BIT(mconf->link_id);
	mlo->resident_links |= BIT(mconf->link_id);
	if (active)
		mlo->traffic_active_links |= BIT(mconf->link_id);
	else
		mlo->traffic_active_links &= ~BIT(mconf->link_id);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_RESIDENT_TRACE: fn=note_bss link=%u active=%d negotiated_links=0x%x resident_links=0x%x\n",
		      mconf->link_id, active, mlo->negotiated_links, mlo->resident_links);
}

static void
mt7927_mlo_note_sta(struct mt792x_vif *mvif, struct mt792x_link_sta *mlink,
		    bool active)
{
	struct mt7927_mlo_state *mlo = &mvif->mt7927_mlo;
	struct mt7927_mlo_link_state *state = &mlo->link[mlink->wcid.link_id];

	state->negotiated = true;
	state->resident = true;
	state->traffic_active = active;
	state->link_ready = !!mvif->sta.mlo_link_ready_jiffies[mlink->wcid.link_id];
	state->link_id = mlink->wcid.link_id;
	state->peer_wcid = mlink->wcid.idx;
	state->ptk_synced = mlink->wcid.hw_key_idx != 0xff;
	state->group_synced = mlink->wcid.hw_key_idx != 0xff ||
			      mlink->wcid.hw_key_idx2 != 0xff;

	mlo->negotiated_links |= BIT(mlink->wcid.link_id);
	mlo->resident_links |= BIT(mlink->wcid.link_id);
	if (active)
		mlo->traffic_active_links |= BIT(mlink->wcid.link_id);
	else
		mlo->traffic_active_links &= ~BIT(mlink->wcid.link_id);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_RESIDENT_TRACE: fn=note_sta link=%u active=%d negotiated_links=0x%x resident_links=0x%x\n",
		      mlink->wcid.link_id, active, mlo->negotiated_links, mlo->resident_links);
}

/* Single reset point for the post-key-switch standalone-ROC experiment's
 * pending-operation state (peer-owned, on struct mt792x_sta -- see the
 * struct's own comment for why), so every exit path (structural failure,
 * key install failure, ROC rejection/timeout, activation failure, success,
 * link retirement, STA disassociation teardown, or a stale unconsumed
 * record from an earlier transition_generation) clears it the same way.
 */
void
mt7927_mlo_pending_clear(struct mt792x_sta *msta)
{
	msta->postkey_pending = false;
	msta->postkey_sublink_id = 0;
	msta->postkey_survivor_id = 0;
	msta->postkey_generation = 0;
	msta->postkey_roc_attempted = false;
}

static struct mt792x_bss_conf *
mt7927_mlo_resident_bss(struct mt792x_dev *dev, struct mt792x_vif *mvif,
			unsigned int link_id)
{
	return rcu_dereference_protected(mvif->link_conf[link_id],
					 lockdep_is_held(&dev->mt76.mutex));
}

static struct mt792x_link_sta *
mt7927_mlo_resident_sta(struct mt792x_dev *dev, struct mt792x_sta *msta,
			unsigned int link_id)
{
	return rcu_dereference_protected(msta->link[link_id],
					 lockdep_is_held(&dev->mt76.mutex));
}

static void
mt7927_mlo_deactivate_wcid(struct mt792x_dev *dev, struct mt792x_link_sta *mlink)
{
	struct mt76_wcid *wcid = &mlink->wcid;

	mt76_connac_free_pending_tx_skbs(&dev->pm, wcid);
	WRITE_ONCE(mlink->mlo_await_valid_rx, false);
	wcid->link_valid = false;
}

/* TEST-ONLY band-context placement A/B for 5+6 GHz pairs: swap which link
 * receives context 0 and context 1 (default keeps link1=0/link2=1).
 */
static bool mt7925_test_mt7927_pair56_reverse;
module_param_named(test_mt7927_pair56_reverse,
		   mt7925_test_mt7927_pair56_reverse, bool, 0644);
MODULE_PARM_DESC(test_mt7927_pair56_reverse,
		 "TEST-ONLY: give the 6 GHz link of a 5+6 GHz pair band context 0 and the 5 GHz link context 1 (default N)");

/* TEST-ONLY: give BOTH links of a 5+6 GHz pair band context 1. The firmware
 * grants both links dbdcband=1 in every pair ROC, while the stock host
 * placement programs the 5 GHz link on context 0.
 */
static bool mt7925_test_mt7927_pair56_same_slot;
module_param_named(test_mt7927_pair56_same_slot,
		   mt7925_test_mt7927_pair56_same_slot, bool, 0644);
MODULE_PARM_DESC(test_mt7927_pair56_same_slot,
		 "TEST-ONLY: give both links of a 5+6 GHz pair band context 1 (default N)");

static u8
mt7927_mlo_pair_band_idx(struct ieee80211_vif *vif, u16 new_links,
			 unsigned int link_id)
{
	struct ieee80211_bss_conf *link_conf;
	enum nl80211_band band;
	unsigned int first;

	link_conf = mt792x_vif_to_bss_conf(vif, link_id);
	if (!link_conf || !link_conf->chanreq.oper.chan)
		return 0xff;

	band = link_conf->chanreq.oper.chan->band;
	if (band == NL80211_BAND_2GHZ)
		return 0;

	first = __ffs(new_links);
	link_conf = mt792x_vif_to_bss_conf(vif, first);
	if (link_conf && link_conf->chanreq.oper.chan &&
	    link_conf->chanreq.oper.chan->band == NL80211_BAND_2GHZ)
		return 1;

	/* For 5+6 GHz, preserve deterministic link1=0/link2=1 placement. */
	if (READ_ONCE(mt7925_test_mt7927_pair56_same_slot))
		return 1;
	if (READ_ONCE(mt7925_test_mt7927_pair56_reverse))
		return link_id == first;
	return link_id != __ffs(new_links);
}

static int
mt7927_mlo_reassign_band(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			 struct mt792x_vif *mvif, unsigned int link_id,
			 u8 band_idx)
{
	struct mt792x_bss_conf *mconf;
	struct mt792x_link_sta *bmc;
	struct mt792x_link_sta *peer = NULL;
	struct ieee80211_bss_conf *link_conf;
	struct ieee80211_link_sta *link_sta = NULL;
	u8 old_band, old_bmc_phy;
	bool old_bmc_valid, old_peer_valid = false, restore = false;
	int err;

	mconf = mt7927_mlo_resident_bss(dev, mvif, link_id);
	bmc = mt7927_mlo_resident_sta(dev, &mvif->sta, link_id);
	link_conf = mt792x_vif_to_bss_conf(vif, link_id);
	if (!mconf || !bmc || !link_conf || band_idx == 0xff)
		return -EINVAL;
	if (mconf->mt76.band_idx == band_idx)
		return 0;

	if (mvif->wep_sta)
		peer = mt7927_mlo_resident_sta(dev, mvif->wep_sta, link_id);
	if (peer) {
		struct ieee80211_sta *sta =
			wcid_to_sta(&mvif->wep_sta->deflink.wcid);

		if (!sta)
			return -EINVAL;
		link_sta = mt792x_sta_to_link_sta(vif, sta, link_id);
		if (!link_sta)
			return -EINVAL;
	}

	old_band = mconf->mt76.band_idx;
	old_bmc_phy = bmc->wcid.phy_idx;
	old_bmc_valid = bmc->wcid.link_valid;
	bmc->wcid.link_valid = false;
	mt76_connac_free_pending_tx_skbs(&dev->pm, &bmc->wcid);

	if (peer) {
		old_peer_valid = peer->wcid.link_valid;
		peer->wcid.link_valid = false;
		mt76_connac_free_pending_tx_skbs(&dev->pm, &peer->wcid);

		/* TEST-ONLY: drop the peer STA_REC while its BSS is still on
		 * the old radio, so the band move never carries a live peer.
		 */
		if (READ_ONCE(mt7925_test_mt7927_migrate_rebuild) && link_sta) {
			err = mt7925_mcu_sta_update(dev, link_sta, vif, peer,
						    false,
						    MT76_STA_INFO_STATE_NONE, NULL);
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_MIGRATE_OLDBAND_REMOVE: link=%u wcid=%u old_band=%u ret=%d\n",
				      link_id, peer->wcid.idx,
				      mconf->mt76.band_idx, err);
		}
	}

	/* Every link of an MLD shares one OMAC/device identity. Disabling the
	 * old per-link DEV_INFO before changing its band can therefore tear down
	 * the live MLD device, after which firmware stops answering the recreate
	 * command. Update the resident link in place instead.
	 */
	mconf->mt76.band_idx = band_idx;
	bmc->wcid.phy_idx = band_idx;

	err = mt76_connac_mcu_uni_add_dev(&dev->mphy, link_conf,
					  &mconf->mt76, &bmc->wcid, true);
	if (!err)
		err = mt7925_mcu_add_bss_info(&dev->phy, mconf->mt76.ctx,
						 link_conf, link_sta, true);
	if (!err) {
		mvif->mt7927_mlo.link[link_id].fw_context_slot = band_idx;
		/* Only a peer resident during this move needs a later rebind.
		 * A fresh STA created on the new BSS must not inherit the work.
		 */
		mvif->mt7927_mlo.link[link_id].band_moved = peer && old_band != band_idx;
		bmc->wcid.link_valid = old_bmc_valid;

		/* Preserve the legacy optional early restoration experiment. Host
		 * PHY equality alone is not proof of a completed peer rebind:
		 * mt7927_mlo_refresh_reassigned_sta() must still consume the pending
		 * move before treating firmware programming as complete. With this
		 * experiment off, that refresh restores eligibility on success.
		 */
		restore = mt7925_mlo_peer_restore_valid_after_move(READ_ONCE(mt7925_mlo_restore_peer_valid),
								   !!peer,
								   peer ? peer->wcid.phy_idx : 0,
								   band_idx);
		if (restore)
			peer->wcid.link_valid = old_peer_valid;
		if (peer && (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
			printk(KERN_INFO
			       "MLO_PEER_VALID: site=reassign_band link=%u wcid=%u phy_idx=%u old=%d now=%d restore=%d band=%u->%u\n",
			       link_id, peer->wcid.idx, peer->wcid.phy_idx,
			       old_peer_valid, peer->wcid.link_valid, restore,
			       old_band, band_idx);

		/* The BMC/group-key WCID just moved to a different physical
		 * radio the same way the real peer did above, but unlike the
		 * peer there is no per-station rebind call in this path at
		 * all -- only the host-side phy_idx field changed. Live trace
		 * (ARP resolution failing -- "INCOMPLETE"/"FAILED" neighbor
		 * entries on this link, distinct from and in addition to the
		 * WLAN_IDX=4095 unicast symptom) proved firmware's BMC WTBL
		 * entry stays bound to the OLD radio. mt7925_mlo_group_key_rebind()
		 * exists for exactly this kind of reinstall but no-ops here on
		 * its own idempotency check (it only compares against
		 * wcid->hw_key_idx, which this migration never touched) --
		 * invalidate it first so the check sees real work to do and
		 * issues a genuine MCU key reinstall on the new radio.
		 */
		bmc->wcid.hw_key_idx = 255;
		bmc->wcid.hw_key_idx2 = 255;
		err = mt7925_mlo_group_key_rebind(dev, vif, link_id, NULL);
		if (err) {
			/* The DEV/BSS move itself already succeeded and, per the
			 * comment above, must not be compounded with a
			 * disable/recreate rollback here. But leaving
			 * hw_key_idx=255 and silently reporting success would
			 * let a caller believe this link is fully migrated when
			 * its group key is not actually usable on the new
			 * radio. Propagate the failure: hw_key_idx staying 255
			 * is the true state, and the steering validate/rollback
			 * state machine already knows how to fail a transition
			 * whose low-level band move happened but whose link
			 * isn't actually healthy, reverting active_links back
			 * to the previous pair.
			 */
			return err;
		}
		return 0;
	}

	/* A timeout means firmware may already be stalled. Do not compound it
	 * with teardown/recreate rollback commands; restore only host state.
	 */
	mconf->mt76.band_idx = old_band;
	bmc->wcid.phy_idx = old_bmc_phy;

	bmc->wcid.link_valid = old_bmc_valid;
	if (peer)
		peer->wcid.link_valid = old_peer_valid;
	return err;
}

static int
mt7927_mlo_reassign_active_bands(struct mt792x_dev *dev,
				 struct ieee80211_vif *vif,
				 struct mt792x_vif *mvif, u16 new_links,
				 u16 activate_links)
{
	unsigned long links = new_links;
	unsigned int link_id;
	int err;

	if (hweight16(new_links) != 2)
		return -EINVAL;

	for_each_set_bit(link_id, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *mconf =
			mt7927_mlo_resident_bss(dev, mvif, link_id);
		u8 band_idx = mt7927_mlo_pair_band_idx(vif, new_links, link_id);
		bool same_band;

		if (!mconf)
			return -EINVAL;
		same_band = mconf->mt76.band_idx == band_idx;

		err = mt7927_mlo_reassign_band(dev, vif, mvif, link_id,
					       band_idx);
		if (err)
			return err;

		/* A resident link can retain this band number while the radio's
		 * DEV address belongs to the link being retired. Reclaim only the
		 * incoming slot, after selecting its band. A real band move already
		 * sends this enable; never disable the shared MLD device here.
		 */
		if (same_band && (activate_links & BIT(link_id))) {
			struct mt792x_link_sta *bmc =
				mt7927_mlo_resident_sta(dev, &mvif->sta, link_id);
			struct ieee80211_bss_conf *link_conf =
				mt792x_vif_to_bss_conf(vif, link_id);

			if (!bmc || !link_conf)
				return -EINVAL;
			err = mt76_connac_mcu_uni_add_dev(&dev->mphy, link_conf,
							  &mconf->mt76,
							  &bmc->wcid, true);
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_SLOT_DEV_ACTIVATE: link=%u bss=%u band=%u ret=%d\n",
				       link_id, mconf->mt76.idx, band_idx, err);
			if (err)
				return err;
		}
	}

	return 0;
}

static int
mt7927_mlo_rebuild_peer_assoc_order(struct mt792x_dev *dev,
				    struct ieee80211_vif *vif,
				    struct ieee80211_sta *sta,
				    struct ieee80211_link_sta *link_sta,
				    struct mt792x_link_sta *mlink,
				    unsigned int link_id, bool retry)
{
	int err;

	if (retry)
		return 0;

	err = mt7925_mcu_sta_update(dev, link_sta, vif, mlink, false,
				    MT76_STA_INFO_STATE_NONE, NULL);
	if (!err)
		err = mt7925_mcu_sta_update(dev, link_sta, vif, mlink, true,
					    MT76_STA_INFO_STATE_NONE, NULL);
	mlink->wcid.link_valid = true;
	if (!err)
		err = mt7925_mcu_sta_authorize(dev, link_sta, vif, mlink);
	if (!err) {
		mlink->wcid.hw_key_idx = 255;
		err = mt7925_mlo_reconcile_link(dev, vif, sta, mlink, link_id);
	}
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_MIGRATE_REBUILD: link=%u wcid=%u phy_idx=%u hw_key_idx=%d ret=%d\n",
		      link_id, mlink->wcid.idx, mlink->wcid.phy_idx,
		      mlink->wcid.hw_key_idx, err);
	return err;
}

static int
mt7927_mlo_recreate_authorize_peer(struct mt792x_dev *dev,
				   struct ieee80211_vif *vif,
				   struct ieee80211_sta *sta,
				   struct mt792x_sta *msta,
				   struct ieee80211_link_sta *link_sta,
				   struct mt792x_link_sta *peer,
				   unsigned int link_id, bool retry,
				   bool defer_authorize)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	const char *create_stage = retry ? "retry_create" : "create";
	const char *authorize_stage = retry ? "retry_authorize" : "authorize";
	int err;

	if (READ_ONCE(mt7925_test_mt7927_migrate_rebuild))
		return mt7927_mlo_rebuild_peer_assoc_order(dev, vif, sta, link_sta,
							   peer, link_id, retry);

	err = mt7925_mcu_sta_rebind_phy(dev, link_sta, vif, peer);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_FW_PEER_LIFECYCLE: generation=%u transition_from=0x%x transition_to=0x%x stage=%s link_id=%u wcid=%u phy_idx=%u state=%u newly=1 hw_key_idx=%u link_valid=%u ready_ts=%lu ret=%d\n",
		       mvif->mt7927_mlo.transition_generation,
		       mvif->mt7927_mlo.migration_from_links,
		       mvif->mt7927_mlo.migration_to_links, create_stage,
		       link_id, peer->wcid.idx, peer->wcid.phy_idx,
		       MT76_STA_INFO_STATE_NONE, peer->wcid.hw_key_idx,
		       peer->wcid.link_valid,
		       msta->mlo_link_ready_jiffies[link_id], err);
	if (err)
		return err;

	peer->wcid.hw_key_idx = 255;
	err = mt7925_mlo_reconcile_link(dev, vif, sta, peer, link_id);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_FW_PEER_LIFECYCLE: generation=%u transition_from=0x%x transition_to=0x%x stage=restore link_id=%u wcid=%u phy_idx=%u state=%u newly=0 hw_key_idx=%u link_valid=%u ready_ts=%lu ret=%d\n",
		       mvif->mt7927_mlo.transition_generation,
		       mvif->mt7927_mlo.migration_from_links,
		       mvif->mt7927_mlo.migration_to_links, link_id,
		       peer->wcid.idx, peer->wcid.phy_idx,
		       MT76_STA_INFO_STATE_NONE, peer->wcid.hw_key_idx,
		       peer->wcid.link_valid,
		       msta->mlo_link_ready_jiffies[link_id], err);
	if (err)
		return err;

	if (defer_authorize && !retry) {
		msta->mlo_defer_auth_links |= BIT(link_id);
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			       "MLO_FW_PEER_LIFECYCLE: generation=%u transition_from=0x%x transition_to=0x%x stage=authorize_deferred link_id=%u wcid=%u phy_idx=%u state=%u newly=0 hw_key_idx=%u link_valid=%u ready_ts=%lu ret=0\n",
			       mvif->mt7927_mlo.transition_generation,
			       mvif->mt7927_mlo.migration_from_links,
			       mvif->mt7927_mlo.migration_to_links, link_id,
			       peer->wcid.idx, peer->wcid.phy_idx,
			       MT76_STA_INFO_STATE_NONE, peer->wcid.hw_key_idx,
			       peer->wcid.link_valid,
			       msta->mlo_link_ready_jiffies[link_id]);
		goto publish;
	}

	err = mt7925_mcu_sta_authorize(dev, link_sta, vif, peer);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_FW_PEER_LIFECYCLE: generation=%u transition_from=0x%x transition_to=0x%x stage=%s link_id=%u wcid=%u phy_idx=%u state=%u newly=0 hw_key_idx=%u link_valid=%u ready_ts=%lu ret=%d\n",
		       mvif->mt7927_mlo.transition_generation,
		       mvif->mt7927_mlo.migration_from_links,
		       mvif->mt7927_mlo.migration_to_links, authorize_stage,
		       link_id, peer->wcid.idx, peer->wcid.phy_idx,
		       MT76_STA_INFO_STATE_ASSOC, peer->wcid.hw_key_idx,
		       peer->wcid.link_valid,
		       msta->mlo_link_ready_jiffies[link_id], err);
	if (err)
		return err;

publish:
	peer->wcid.link_valid = true;
	return 0;
}

/* Shared bounded RX-convergence policy: the exact backoff schedule
 * mt7927_mlo_refresh_reassigned_sta()'s own inline loop already uses
 * (kept there unmodified -- tests/mt7925/mlo_peer_authorization_lifecycle_contract.sh
 * greps for a literal "for (attempt = 0;" loop calling
 * mt7927_mlo_recreate_authorize_peer() inside that function's own body, so
 * that loop is not moved here). New call sites needing the same policy
 * (fresh link admission) use this instead of inventing their own.
 *
 * This performs NO lifecycle work itself and never publishes readiness --
 * it only tells the caller whether real RX progress was observed against
 * baseline_rx. retry_fn, when non-NULL, is invoked between waits so a
 * caller with a genuine re-stimulus action (reissuing the peer's
 * RESTORE+AUTHORIZE MCU exchange, exactly the kind of control-plane
 * operation mt7927_mlo_recreate_authorize_peer() already performs for
 * migration) can use it. This is never ordinary user data, never an
 * arbitrary injected frame, and never a fabricated counter -- it is the
 * same firmware STA_REC re-sync mechanism already proven live to restore a
 * peer's RX WTBL binding.
 */
typedef int (*mt7927_mlo_link_retry_fn)(void *ctx, unsigned int link_id,
					bool retry);

static int
mt7927_mlo_wait_link_rx_ready(struct mt792x_link_sta *peer,
			      unsigned int link_id, u32 baseline_rx,
			      const char *reason,
			      mt7927_mlo_link_retry_fn retry_fn,
			      void *retry_ctx, bool *rx_progress_out)
{
	static const unsigned int backoff_ms[] = { 50, 100, 200, 400 };
	unsigned int attempt;
	bool rx_progress = false;

	for (attempt = 0; attempt < ARRAY_SIZE(backoff_ms); attempt++) {
		/* Unkeyed peer: no datapath proof possible; leave it waiting
		 * for late valid RX instead of re-authorizing mid-handshake.
		 */
		if (peer->wcid.hw_key_idx == (u8)-1)
			break;
		msleep(backoff_ms[attempt]);
		rx_progress = peer->mlo_valid_rx_packets != baseline_rx;

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_LINK_ADMIT_CONVERGE: reason=%s link=%u attempt=%u rx_baseline=%u rx_now=%u rx_progress=%d\n",
			      reason, link_id, attempt, baseline_rx,
			      peer->mlo_valid_rx_packets, rx_progress);

		if (rx_progress)
			break;

		if (retry_fn) {
			int err = retry_fn(retry_ctx, link_id, true);

			if (err) {
				*rx_progress_out = false;
				return err;
			}
		}
	}

	*rx_progress_out = rx_progress;
	return 0;
}

static int
mt7927_mlo_refresh_reassigned_sta(struct mt792x_dev *dev,
				  struct ieee80211_vif *vif,
				  struct ieee80211_sta *sta,
				  struct mt792x_sta *msta, u16 new_links,
				  bool defer_authorize)
{
	unsigned long links = new_links;
	unsigned int link_id;

	for_each_set_bit(link_id, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_link_sta *link_sta;
		struct mt792x_link_sta *peer;
		struct mt792x_bss_conf *mconf;
		int err;

		peer = mt7927_mlo_resident_sta(dev, msta, link_id);
		mconf = mt7927_mlo_resident_bss(dev,
					       (struct mt792x_vif *)vif->drv_priv,
					       link_id);
		if (!peer || !mconf)
			continue;

		/* VIF grow runs before incoming STA allocation. The original peer
		 * can still have creation-time phy_idx=0 on BSS band1: that does
		 * not justify newly=1 or authorizing a not-yet-existing pair.
		 * Only a successful DEV/BSS move arms this pending refresh. Keep
		 * it pending across errors, including when host phy_idx already
		 * equals the destination after a partially completed attempt.
		 */
		if (!((struct mt792x_vif *)vif->drv_priv)->mt7927_mlo.link[link_id].band_moved)
			continue;

		link_sta = mt792x_sta_to_link_sta(vif, sta, link_id);
		if (!link_sta)
			return -EINVAL;

		peer->wcid.phy_idx = mconf->mt76.band_idx;

		/* Withhold this link from ordinary TX selection for the entire
		 * destructive migration below (rebind_phy/reconcile_link can
		 * flip wcid.link_valid=true well before real RX is proven --
		 * see mt7925_mlo_reconcile_link()'s call site further down).
		 * mt7925_mlo_link_tx_stable() (mt792x_core.c) is the existing
		 * gate every TX candidate path already checks; it was only
		 * ever armed once at first-ever WCID creation (MLO_LINK_READY,
		 * main.c) and never rearmed here, so a migrating survivor read
		 * as "long stable" from stale pre-migration history the
		 * instant link_valid flipped true -- proven live (2026-09-12):
		 * bulk TCP dispatch collapsed from ~8000/s to ~1/s within the
		 * same second as this migration started, ~10s before
		 * MLO_STEER_COMMIT even ran. Clearing to 0 here and restamping
		 * to jiffies only after the convergence loop below finishes
		 * closes that window using the exact mechanism that already
		 * protects brand-new link admission, uniformly.
		 */
		msta->mlo_link_ready_jiffies[link_id] = 0;

		/* This peer is moving to a different physical radio. Any
		 * pre-migration TX-liveness history (mlo_tx_liveness.h) was
		 * proof about the OLD radio only -- carrying it forward would
		 * let a migrated link with stale good history outrank a
		 * genuinely untested one in the deterministic TX selector
		 * before it has proven anything on the NEW radio. Unlike a
		 * full peer recreation (which naturally zeroes a freshly
		 * allocated mlink), a migration reuses this same object, so
		 * this reset has to be explicit.
		 */
		peer->tx_liveness_rx_seen = peer->mlo_valid_rx_packets;
		peer->tx_liveness_tx_seen = peer->wcid.stats.tx_packets;
		peer->tx_liveness_rx_ever_seen = false;
		peer->tx_liveness_rx_progress_jiffies = 0;
		/* Security-valid RX proof from the OLD radio must not satisfy
		 * the sole-secure-link continuity gate for the new one.
		 */
		WRITE_ONCE(peer->mlo_valid_rx_jiffies, 0);

		/* Ordinary STATE_ASSOC "update" semantics leave firmware's
		 * RX-side WLAN_IDX/WTBL binding on the peer's OLD radio --
		 * proven live as a WLAN_IDX=4095 sentinel on every RX frame
		 * for this peer after a band migration, even though TX and
		 * the command ack both succeed. Force a full rebind.
		 *
		 * Live trace across many runs showed the firmware-side RX
		 * WTBL move does not complete on a fixed schedule relative to
		 * the ack -- one run recovered within ~1s, another stayed
		 * broken for ~14s despite an identical single blind resend at
		 * +50ms. A fixed one-shot retry cannot cover both. Retry on
		 * real evidence instead: reissue the rebind and check for an
		 * actual RX packet landing on this wcid between attempts,
		 * backing off geometrically, bounded so this can never become
		 * an open-ended stall. Exhausting the budget without evidence
		 * is not fatal -- it fails through to the existing VERIFY/
		 * rollback state machine, which is the real safety net; this
		 * loop only exists to make the common case converge fast
		 * instead of waiting out the whole VERIFY window every time.
		 */
		/* force_newly recreates the peer WTBL/STA_REC on a different
		 * physical radio. The PTK index is host bookkeeping, not proof that
		 * the recreated firmware entry inherited its key. Keeping the old
		 * index makes mt7925_mlo_reconcile_link() take its idempotent
		 * shortcut, leaving protected RX unable to become an MSDU even though
		 * the AP still attributes the frame to the correct peer. Invalidate
		 * the marker and reinstall the existing per-MLD PTK before making the
		 * migrated WCID visible to traffic. This mirrors the BMC/group-key
		 * migration directly above in mt7927_mlo_reassign_band().
		 */
		err = mt7927_mlo_recreate_authorize_peer(dev, vif, sta, msta,
							link_sta, peer, link_id,
							false, defer_authorize);
		if (err)
			return err;

		{
			static const unsigned int backoff_ms[] = { 50, 100, 200, 400 };
			u32 baseline_rx = peer->mlo_valid_rx_packets;
			unsigned int attempt;

			for (attempt = 0; attempt < ARRAY_SIZE(backoff_ms); attempt++) {
				/* No PTK yet (initial association): data RX
				 * cannot prove anything, and re-creating the
				 * peer would disrupt the 4-way handshake.
				 */
				if (peer->wcid.hw_key_idx == (u8)-1)
					break;
				msleep(backoff_ms[attempt]);
				if (peer->mlo_valid_rx_packets != baseline_rx)
					break;

				err = mt7927_mlo_recreate_authorize_peer(dev, vif,
								sta, msta, link_sta,
								peer, link_id, true,
								false);
				if (err)
					return err;
			}

			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_PHY_REBIND_CONVERGE: link=%u attempts=%u rx_progress=%d\n",
				      link_id, attempt,
				      peer->mlo_valid_rx_packets != baseline_rx);
		}

		/* Only now -- after the convergence loop above has actually run
		 * -- restamp this link's TX-readiness timestamp, arming the
		 * existing MT7925_MLO_TX_STABILIZE_MS grace window from this
		 * point rather than from stale pre-migration history. An error
		 * return above (reconcile_link/rebind_phy failing) skips this
		 * entirely, leaving the link correctly withheld (ready_ts stays
		 * 0) until a later successful migration attempt reaches here.
		 */
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_LINK_TX_READY_REARM: link=%u\n", link_id);
		msta->mlo_link_ready_jiffies[link_id] = jiffies;
		((struct mt792x_vif *)vif->drv_priv)->mt7927_mlo.link[link_id].band_moved = false;
	}

	return 0;
}

static int
mt7927_mlo_add_bss_link(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			struct mt792x_vif *mvif, unsigned int link_id,
			unsigned long *created)
{
	struct mt792x_bss_conf *mconf;
	struct mt792x_link_sta *mlink;
	struct ieee80211_bss_conf *link_conf;
	int err;

	mconf = mt7927_mlo_resident_bss(dev, mvif, link_id);
	mlink = mt7927_mlo_resident_sta(dev, &mvif->sta, link_id);
	if (mconf && mlink) {
		/* Resident structures can outlive the firmware context: a shrink
		 * removes the BSS and its vif-mask bit but keeps them, and the JOIN
		 * built for the link is then refused. Restore the context on the
		 * same structures; the link is not new, so it is not recorded as
		 * created (the error unwind frees created links).
		 */
		if (!is_mt7927(&dev->mt76) ||
		    mt7927_mlo_link_ctx_installed(dev, mvif, link_id)) {
			mt7927_mlo_note_bss(mvif, mconf, mlink, true);
			return 0;
		}
		link_conf = mt792x_vif_to_bss_conf(vif, link_id);
		if (!link_conf)
			return -EINVAL;
		err = mt7925_mac_link_bss_add(dev, link_conf, mlink);
		if (err)
			return err;
		mt7927_mlo_note_bss(mvif, mconf, mlink, true);
		return 0;
	}

	link_conf = mt792x_vif_to_bss_conf(vif, link_id);
	if (!link_conf)
		return -EINVAL;

	mconf = devm_kzalloc(dev->mt76.dev, sizeof(*mconf), GFP_KERNEL);
	mlink = devm_kzalloc(dev->mt76.dev, sizeof(*mlink), GFP_KERNEL);
	if (!mconf || !mlink)
		return -ENOMEM;

	mconf->link_id = link_id;
	mconf->vif = mvif;
	mlink->wcid.link_id = link_id;
	mlink->wcid.link_valid = false;
	mlink->wcid.def_wcid = &mvif->sta.deflink.wcid;

	rcu_assign_pointer(mvif->link_conf[link_id], mconf);
	rcu_assign_pointer(mvif->sta.link[link_id], mlink);

	err = mt7925_mac_link_bss_add(dev, link_conf, mlink);
	if (err)
		goto err_clear;

	*created |= BIT(link_id);
	mvif->valid_links |= BIT(link_id);
	mt7927_mlo_note_bss(mvif, mconf, mlink, true);
	return 0;

err_clear:
	rcu_assign_pointer(mvif->link_conf[link_id], NULL);
	rcu_assign_pointer(mvif->sta.link[link_id], NULL);
	devm_kfree(dev->mt76.dev, mconf);
	devm_kfree(dev->mt76.dev, mlink);
	return err;
}

int mt7927_mlo_prime_dormant_link(struct mt792x_dev *dev,
				  struct ieee80211_vif *vif,
				  struct mt792x_vif *mvif,
				  unsigned int link_id)
{
	struct mt792x_bss_conf *mconf;
	struct mt792x_link_sta *mlink;
	struct ieee80211_bss_conf *link_conf;
	int err;

	if ((!READ_ONCE(mt7925_test_mt7927_dormant_rssi_bss) &&
	     !READ_ONCE(mt7925_test_mt7927_mlo_join3)) ||
	    !is_mt7927(&dev->mt76) || !ieee80211_vif_is_mld(vif) ||
	    !(vif->valid_links & BIT(link_id)))
		return 0;

	mconf = mt7927_mlo_resident_bss(dev, mvif, link_id);
	mlink = mt7927_mlo_resident_sta(dev, &mvif->sta, link_id);
	if (mconf && mlink)
		return 0;

	link_conf = mt792x_vif_to_bss_conf(vif, link_id);
	if (!link_conf)
		return -EINVAL;

	mconf = devm_kzalloc(dev->mt76.dev, sizeof(*mconf), GFP_KERNEL);
	mlink = devm_kzalloc(dev->mt76.dev, sizeof(*mlink), GFP_KERNEL);
	if (!mconf || !mlink)
		return -ENOMEM;

	mconf->link_id = link_id;
	mconf->vif = mvif;
	mlink->wcid.link_id = link_id;
	mlink->wcid.link_valid = false;
	mlink->wcid.def_wcid = &mvif->sta.deflink.wcid;

	rcu_assign_pointer(mvif->link_conf[link_id], mconf);
	rcu_assign_pointer(mvif->sta.link[link_id], mlink);

	err = mt7925_mac_link_bss_add(dev, link_conf, mlink);
	if (err) {
		rcu_assign_pointer(mvif->link_conf[link_id], NULL);
		rcu_assign_pointer(mvif->sta.link[link_id], NULL);
		devm_kfree(dev->mt76.dev, mconf);
		devm_kfree(dev->mt76.dev, mlink);
		return err;
	}

	mt7927_mlo_note_bss(mvif, mconf, mlink, false);

	dev_warn(dev->mt76.dev,
		"MLO_DORMANT_BSS: link_id=%u primed bss_idx=%u wcid=%u (no traffic/keys/ROC)\n",
		link_id, mconf->mt76.idx, mlink->wcid.idx);

	return 0;
}

/* Production delta radio acquisition for a pair transition. Default on;
 * off restores the legacy replacement JOIN that re-requests the survivor
 * (never granted while the survivor's slot is held).
 */
static bool mt7925_mlo_delta_acquire = true;
module_param_named(mlo_delta_acquire, mt7925_mlo_delta_acquire, bool, 0644);
MODULE_PARM_DESC(mlo_delta_acquire,
		 "MT7927 pair transition: acquire only the incoming link's radio slot (default Y)");

/* Survivor ownership handoff experiment (default off = existing production
 * path): before the incoming link is acquired, release the survivor's held
 * channel-manager slot, JOIN the same survivor again under a fresh token,
 * recover its security-valid RX, and only then request the incoming link.
 */
static bool mt7925_mlo_survivor_rejoin;
module_param_named(mlo_survivor_rejoin, mt7925_mlo_survivor_rejoin, bool, 0644);
MODULE_PARM_DESC(mlo_survivor_rejoin,
		 "MT7927 pair transition: release + re-JOIN + recover the survivor before acquiring the incoming link (default N)");

/* TEST-ONLY replay of the Sep-18 mid-connection cell (default off): forces the
 * legacy full two-TLV JOIN (the delta path is bypassed) and, once firmware
 * grants it, recovers the survivor with today's peer lifecycle before the
 * incoming link's normal lifecycle runs. It must be paired with
 * mlo_roc_release bits 2|8 (release-all at shrink, as Sep-18 did) and
 * mlo_delta_acquire=N; the run harness checks that.
 */
static bool mt7925_mlo_sep18_replay;
module_param_named(mlo_sep18_replay, mt7925_mlo_sep18_replay, bool, 0644);
MODULE_PARM_DESC(mlo_sep18_replay,
		 "MT7927 test-only: replay the Sep-18 release-all + full 2-TLV JOIN, then recover the survivor peer (default N)");

/* TEST-ONLY (default off; used with mlo_sep18_replay): after the full JOIN
 * re-grants the radio group, rebind the firmware security state of BOTH
 * active links in one ordered pass (see mt7927_mlo_security_rebind_pair()).
 */
static bool mt7925_mlo_security_rebind;
module_param_named(mlo_security_rebind, mt7925_mlo_security_rebind, bool, 0644);
MODULE_PARM_DESC(mlo_security_rebind,
		 "MT7927 test-only: after a radio-group re-grant, invalidate and rebind PTK/GTK/STA_REC_MLD for both active links (default N)");

/* TEST-ONLY (default off; used with mlo_security_rebind): before the security
 * rebind, rebuild the DEV_INFO/BSS_INFO parent context of BOTH active links
 * against the new grants (see mt7927_mlo_regrant_parent_rebuild()).
 */
static bool mt7925_mlo_regrant_parent_rebuild;
module_param_named(mlo_regrant_parent_rebuild, mt7925_mlo_regrant_parent_rebuild,
		   bool, 0644);
MODULE_PARM_DESC(mlo_regrant_parent_rebuild,
		 "MT7927 test-only: after a radio-group re-grant, re-send DEV_INFO then BSS_INFO for both active links before the security rebind (default N)");

/* Destructive behaviour (releasing the security-valid continuity grant) is
 * reachable only behind these explicit test gates, all default off; the
 * production default mlo_roc_release=3 never sets the release-all bit 8.
 */
bool mt7927_mlo_destructive_test_gate(void)
{
	return mt7925_mlo_pt_destructive_gate(READ_ONCE(mt7925_mlo_sep18_replay),
					      READ_ONCE(mt7925_mlo_security_rebind),
					      READ_ONCE(mt7925_mlo_regrant_parent_rebuild),
					      READ_ONCE(mt7925_mlo_survivor_rejoin),
					      READ_ONCE(mt7925_mlo_roc_release));
}
EXPORT_SYMBOL_GPL(mt7927_mlo_destructive_test_gate);

static int mt7927_mlo_link_band(struct ieee80211_vif *vif, unsigned int lid)
{
	struct ieee80211_bss_conf *lc = mt792x_vif_to_bss_conf(vif, lid);

	if (!lc || !lc->chanreq.oper.chan)
		return MT7925_MLO_BAND_5G;

	switch (lc->chanreq.oper.chan->band) {
	case NL80211_BAND_2GHZ:
		return MT7925_MLO_BAND_2G;
	case NL80211_BAND_6GHZ:
		return MT7925_MLO_BAND_6G;
	default:
		return MT7925_MLO_BAND_5G;
	}
}

static void mt7927_mlo_pt_trace(struct ieee80211_vif *vif,
				struct mt792x_vif *mvif, const char *event,
				const char *what, int result)
{
	struct mt7927_mlo_state *m = &mvif->mt7927_mlo;

	if (!(READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
		return;

	/* active_links is the mask really in effect; inside change_vif_links
	 * mac80211 has already OR'ed the added link into vif->active_links
	 * (printed as mac80211_transient) and restores it if the grow fails.
	 */
	printk(KERN_INFO
	       "%s: id=%u %s state=%s kind=%u old=0x%x target=0x%x active_links=0x%x mac80211_transient=0x%x valid_links=0x%x survivor=0x%x removed=0x%x added=0x%x token=%u link=%d req_slot=%u grant_slot=%u result=%d\n",
	       event, m->transition_generation, what,
	       mt7925_mlo_pt_state_name(m->pt_state), m->pt_kind,
	       m->pt_old_links, m->pt_target_links,
	       mt7925_mlo_pt_published_active(vif->active_links, m->pt_added,
					      m->pt_grant_slot != 255 ?
					      m->pt_added : 0),
	       vif->active_links,
	       vif->valid_links, m->pt_survivor, m->pt_removed, m->pt_added,
	       m->pt_token, m->pt_link, m->pt_req_slot, m->pt_grant_slot,
	       result);
}

static void mt7927_mlo_pt_set_state(struct ieee80211_vif *vif,
				    struct mt792x_vif *mvif,
				    enum mt7925_mlo_pt_state to,
				    const char *why)
{
	struct mt7927_mlo_state *m = &mvif->mt7927_mlo;
	bool legal = mt7925_mlo_pt_can_advance(m->pt_state, to);

	m->pt_state = to;
	mt7927_mlo_pt_trace(vif, mvif, "MLO_PAIR_TRANSITION_STATE", why,
			    legal ? 0 : -EINVAL);
}

static void mt7927_mlo_pt_stamp(struct mt792x_vif *mvif, unsigned int idx)
{
	if (idx < ARRAY_SIZE(mvif->mt7927_mlo.pt_ns))
		mvif->mt7927_mlo.pt_ns[idx] = ktime_get_ns();
}

/* us between two stamps of this transition, -1 when either is unset */
static s64 mt7927_mlo_pt_dt_us(struct mt792x_vif *mvif, unsigned int from,
			       unsigned int to)
{
	struct mt7927_mlo_state *m = &mvif->mt7927_mlo;

	if (!m->pt_ns[from] || !m->pt_ns[to])
		return -1;
	return div_s64((s64)(m->pt_ns[to] - m->pt_ns[from]), 1000);
}

static void mt7927_mlo_pt_timeline(struct ieee80211_vif *vif,
				   struct mt792x_vif *mvif, const char *why)
{
	struct mt7927_mlo_state *m = &mvif->mt7927_mlo;

	if (!(READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
		return;

	printk(KERN_INFO
	       "MLO_PAIR_TRANSITION_TIMELINE: id=%u %s old=0x%x target=0x%x T0_mono_us=%llu rel_us T1=%lld T2=%lld T3=%lld T4=%lld T5=%lld T6=%lld T7=%lld T8=%lld T9=%lld | T2-T1=%lld T3-T2=%lld T5-T1=%lld T7-T6=%lld T8-T6=%lld T9-T0=%lld\n",
	       m->transition_generation, why, m->pt_old_links,
	       m->pt_target_links, (unsigned long long)(m->pt_ns[0] / 1000),
	       mt7927_mlo_pt_dt_us(mvif, 0, 1), mt7927_mlo_pt_dt_us(mvif, 0, 2),
	       mt7927_mlo_pt_dt_us(mvif, 0, 3), mt7927_mlo_pt_dt_us(mvif, 0, 4),
	       mt7927_mlo_pt_dt_us(mvif, 0, 5), mt7927_mlo_pt_dt_us(mvif, 0, 6),
	       mt7927_mlo_pt_dt_us(mvif, 0, 7), mt7927_mlo_pt_dt_us(mvif, 0, 8),
	       mt7927_mlo_pt_dt_us(mvif, 0, 9),
	       mt7927_mlo_pt_dt_us(mvif, 1, 2), mt7927_mlo_pt_dt_us(mvif, 2, 3),
	       mt7927_mlo_pt_dt_us(mvif, 1, 5), mt7927_mlo_pt_dt_us(mvif, 6, 7),
	       mt7927_mlo_pt_dt_us(mvif, 6, 8), mt7927_mlo_pt_dt_us(mvif, 0, 9));
}

static void mt7927_mlo_pt_begin(struct ieee80211_vif *vif,
				struct mt792x_vif *mvif, u16 old_links,
				u16 new_links)
{
	struct mt7927_mlo_state *m = &mvif->mt7927_mlo;

	m->pt_state = MT7925_MLO_PT_IDLE;
	m->pt_kind = MT7925_MLO_PT_NONE;
	m->rebind_target_links = 0;
	m->radio_group_reincarnated = false;
	m->pt_token = 0;
	m->pt_link = -1;
	m->pt_req_slot = m->pt_grant_slot = 0xff;
	m->pt_old_links = old_links;
	m->pt_target_links = new_links;
	m->pt_survivor = old_links & new_links;
	m->pt_removed = old_links & ~new_links;
	m->pt_added = new_links & ~old_links;
	memset(m->pt_ns, 0, sizeof(m->pt_ns));
	m->pt_ns[0] = ktime_get_ns();
	mt7927_mlo_pairs_note(vif, new_links, 0, "request", false);
	mt7927_mlo_pt_trace(vif, mvif, "MLO_PAIR_TRANSITION_BEGIN", "begin", 0);
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_PREPARE, "prepare");
}

static void mt7927_mlo_pt_failsafe(struct ieee80211_vif *vif,
				   struct mt792x_vif *mvif,
				   struct mt792x_phy *phy, const char *why,
				   int err)
{
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_FAILED_SAFE, why);
	mt7927_mlo_pairs_note(vif, mvif->mt7927_mlo.pt_target_links,
			      err, why, false);
	mt7927_mlo_pt_trace(vif, mvif, "MLO_PAIR_TRANSITION_FAILSAFE", why, err);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_PAIR_TRANSITION_FAILSAFE: id=%u survivor=0x%x survivor_slot_held=0x%x held_token=%u held_links=0x%x reason=%s err=%d\n",
		       mvif->mt7927_mlo.transition_generation,
		       mvif->mt7927_mlo.pt_survivor,
		       phy->mlo_roc.held_links & mvif->mt7927_mlo.pt_survivor,
		       phy->mlo_roc.held_token, phy->mlo_roc.held_links,
		       why, err);
	mt7927_mlo_pt_timeline(vif, mvif, "failsafe");
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_IDLE, "failsafe_done");
}

static void mt7927_mlo_surv_trace(struct ieee80211_vif *vif,
				  struct mt792x_vif *mvif, const char *event,
				  const char *what, unsigned int link,
				  unsigned int old_token,
				  unsigned int new_token, int bss_idx,
				  struct mt792x_link_sta *peer, int result)
{
	struct mt7927_mlo_state *m = &mvif->mt7927_mlo;

	if (!(READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
		return;

	printk(KERN_INFO
	       "%s: id=%u %s link=%u old_token=%u new_token=%u bss_idx=%d active_links=0x%x valid_links=0x%x wcid=%d hw_key_idx=%d valid_rx=%u result=%d mono_us=%llu rel_T0_us=%lld since_release_us=%lld\n",
	       event, m->transition_generation, what, link, old_token,
	       new_token, bss_idx, vif->active_links, vif->valid_links,
	       peer ? (int)peer->wcid.idx : -1,
	       peer ? (int)peer->wcid.hw_key_idx : -1,
	       peer ? peer->mlo_valid_rx_packets : 0, result,
	       (unsigned long long)(ktime_get_ns() / 1000),
	       (long long)div_s64((s64)(ktime_get_ns() - m->pt_ns[0]), 1000),
	       m->pt_ns[1] ?
	       (long long)div_s64((s64)(ktime_get_ns() - m->pt_ns[1]), 1000) :
	       -1LL);
}

/* Poll the survivor's security-valid RX counter (mlo_valid_rx_packets, the
 * same counter every readiness gate uses) for progress past @baseline.
 */
static bool mt7927_mlo_surv_poll_valid_rx(struct mt792x_link_sta *peer,
					  u32 baseline, unsigned int timeout_ms)
{
	u64 end = ktime_get_ns() + (u64)timeout_ms * NSEC_PER_MSEC;

	do {
		if (READ_ONCE(peer->mlo_valid_rx_packets) != baseline)
			return true;
		usleep_range(3000, 5000);
	} while (ktime_get_ns() < end);

	return READ_ONCE(peer->mlo_valid_rx_packets) != baseline;
}

/* Rebuild the survivor's firmware DEV/BSS/BMC context after the fresh grant.
 * The ROC abort leaves the survivor's host objects untouched but its RX
 * vanishes (rx_decrypt_buckets of the previous run), so the channel/radio
 * binding carried only by DEV_INFO and BSS_INFO(enable => RLM) is re-sent,
 * in the order of the normal link lifecycle (mt7925_mac_link_bss_add ->
 * bss info -> keys) and of the proven in-place band move
 * (mt7927_mlo_reassign_band): DEV_INFO enable, BSS_INFO enable (BASIC, SEC,
 * BMC, QOS, MLD, IFS, HE, COLOR, RLM, MBSSID), then the BMC group keys with
 * the stale hw_key_idx invalidated so a real install is issued. Nothing is
 * removed (every MLD link shares one OMAC, so a DEV disable would tear the
 * live device down) and no further command follows a failed step.
 */
static int mt7927_mlo_pt_reprogram_survivor_context(struct mt792x_dev *dev,
						    struct ieee80211_vif *vif,
						    struct mt792x_vif *mvif,
						    struct mt792x_bss_conf *smconf,
						    struct ieee80211_link_sta *link_sta,
						    struct mt792x_link_sta *peer,
						    unsigned int sl,
						    unsigned int old_token,
						    unsigned int new_token)
{
	struct mt792x_link_sta *bmc = mt7927_mlo_resident_sta(dev, &mvif->sta, sl);
	struct ieee80211_bss_conf *link_conf = mt792x_vif_to_bss_conf(vif, sl);
	struct mt7925_group_key_rebind_result kr = {};
	enum mt7925_mlo_pt_ctx_step step;
	int err = 0;

	if (!bmc || !link_conf)
		return -EINVAL;

	mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_CONTEXT_CREATE", "begin",
			      sl, old_token, new_token, smconf->mt76.idx, peer, 0);

	for (step = mt7925_mlo_pt_ctx_next(MT7925_MLO_PT_CTX_NONE, 0);
	     step != MT7925_MLO_PT_CTX_DONE && step != MT7925_MLO_PT_CTX_FAILED;
	     step = mt7925_mlo_pt_ctx_next(step, err)) {
		switch (step) {
		case MT7925_MLO_PT_CTX_DEV:
			err = mt76_connac_mcu_uni_add_dev(&dev->mphy, link_conf,
							  &smconf->mt76,
							  &bmc->wcid, true);
			mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_DEV",
					      "dev_info_enable", sl, old_token,
					      new_token, smconf->mt76.idx, peer,
					      err);
			break;
		case MT7925_MLO_PT_CTX_BSS:
			/* BASIC + MLD + RLM(channel/band/bw) travel in this one
			 * command; there is no separate MLD command.
			 */
			err = mt7925_mcu_add_bss_info(&dev->phy, smconf->mt76.ctx,
						      link_conf, link_sta, true);
			mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_BSS",
					      "bss_info_enable_basic_mld_rlm",
					      sl, old_token, new_token,
					      smconf->mt76.idx, peer, err);
			break;
		case MT7925_MLO_PT_CTX_BMC_KEYS:
			bmc->wcid.hw_key_idx = 255;
			bmc->wcid.hw_key_idx2 = 255;
			err = mt7925_mlo_group_key_rebind(dev, vif, sl, &kr);
			mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_BMC",
					      "bmc_group_key_rebind", sl,
					      old_token, new_token,
					      smconf->mt76.idx, peer, err);
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_SURVIVOR_GTK: id=%u link=%u keys_seen=%u gtk=%u igtk=%u bigtk=%u already_current=%u installs_attempted=%u installs_succeeded=%u result=%d\n",
				       mvif->mt7927_mlo.transition_generation, sl,
				       kr.keys_seen, kr.gtk_seen, kr.igtk_seen,
				       kr.bigtk_seen, kr.already_current,
				       kr.installs_attempted, kr.installs_succeeded,
				       err);
			break;
		default:
			err = -EINVAL;
			break;
		}
	}

	mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_CONTEXT_CREATE",
			      step == MT7925_MLO_PT_CTX_DONE ? "done" : "failed",
			      sl, old_token, new_token, smconf->mt76.idx, peer,
			      step == MT7925_MLO_PT_CTX_DONE ? 0 : err);
	return step == MT7925_MLO_PT_CTX_DONE ? 0 : (err ? err : -EIO);
}

/* Survivor ownership handoff: RELEASE the survivor's held slot, JOIN the
 * same survivor again under a fresh token, RECOVER its security state if it
 * lost it, and require security-valid RX before the caller may request the
 * incoming link. Any failure is FAILED_SAFE (no incoming request, no
 * reassociation, no extra recovery commands beyond the bounded stages).
 */
static int mt7927_mlo_pt_survivor_handoff(struct mt792x_dev *dev,
					  struct mt792x_phy *phy,
					  struct ieee80211_vif *vif,
					  struct mt792x_vif *mvif,
					  const struct mt7925_mlo_pt_plan *pt)
{
	struct mt7927_mlo_state *m = &mvif->mt7927_mlo;
	unsigned int sl = MT7925_MLO_PT_FFS(pt->survivor);
	struct mt792x_bss_conf *smconf = mt7927_mlo_resident_bss(dev, mvif, sl);
	struct mt792x_sta *msta = mvif->wep_sta;
	struct ieee80211_sta *psta = msta ?
		wcid_to_sta(&msta->deflink.wcid) : NULL;
	struct mt792x_link_sta *peer = msta ?
		mt7927_mlo_resident_sta(dev, msta, sl) : NULL;
	struct ieee80211_link_sta *link_sta = psta ?
		mt792x_sta_to_link_sta(vif, psta, sl) : NULL;
	enum mt7925_mlo_pt_recover_stage stage = MT7925_MLO_PT_RECOVER_NONE;
	unsigned int old_token, held_before, held_after, new_token;
	unsigned long t1_j;
	int err, after_ms;
	bool rx_ok = false;
	u32 baseline;

	if (!smconf || !msta || !psta || !peer || !link_sta ||
	    !peer->wcid.link_valid) {
		mt7927_mlo_pt_failsafe(vif, mvif, phy, "handoff_no_survivor_objects",
				       -EINVAL);
		return -EINVAL;
	}

	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_PREPARE_SURVIVOR_HANDOFF,
				"survivor_handoff");
	/* withhold the survivor from ordinary TX selection until secure RX
	 * is proven again (same gate the destructive migration uses)
	 */
	msta->mlo_link_ready_jiffies[sl] = 0;
	peer->tx_liveness_rx_seen = peer->mlo_valid_rx_packets;
	peer->tx_liveness_tx_seen = peer->wcid.stats.tx_packets;
	peer->tx_liveness_rx_ever_seen = false;
	peer->tx_liveness_rx_progress_jiffies = 0;

	/* RELEASE_SURVIVOR_OWNERSHIP */
	mt7927_mlo_pt_set_state(vif, mvif,
				MT7925_MLO_PT_RELEASE_SURVIVOR_OWNERSHIP,
				"release_survivor_slot");
	t1_j = jiffies;
	mt7927_mlo_pt_stamp(mvif, 1);
	mt7925_mlo_roc_survivor_release(phy, mvif, sl, &old_token, &held_before,
					&held_after);
	mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_RELEASE", "release", sl,
			      old_token, 0, smconf->mt76.idx, peer, 0);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_SURVIVOR_RELEASE: id=%u held_before=0x%x held_after=0x%x survivor_only=%d\n",
		       m->transition_generation, held_before, held_after,
		       held_after == (held_before & ~BIT(sl)));

	/* REJOIN_SURVIVOR: same shape as every proven working JOIN (fresh
	 * token, JOIN, ACQUIRE, firmware-selected dbdcband) for the survivor
	 * link only; the grant must report the survivor's own slot.
	 */
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_REJOIN_SURVIVOR,
				"rejoin_survivor");
	new_token = phy->roc_token_id + 1;
	mt7927_mlo_pt_stamp(mvif, 2);
	mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_REJOIN_REQ", "req", sl,
			      old_token, new_token, smconf->mt76.idx, peer, 0);
	err = mt7925_mlo_roc_delta_acquire(phy, smconf, BIT(sl),
					   pt->survivor_slot,
					   m->transition_generation);
	mt7927_mlo_pt_stamp(mvif, 3);
	mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_REJOIN_GRANT", "grant", sl,
			      old_token, phy->roc_token_id, smconf->mt76.idx,
			      peer, err);
	if (err) {
		msta->mlo_link_ready_jiffies[sl] = jiffies;
		mt7927_mlo_pt_failsafe(vif, mvif, phy,
				       err == -ETIMEDOUT ?
				       "survivor_rejoin_no_grant" :
				       "survivor_rejoin_rejected", err);
		return err;
	}

	/* Did the survivor keep its security state? A valid frame seen at
	 * least MT7925_MLO_PT_CM_SETTLE_MS after the release (firmware flips
	 * a released survivor to cm=1 within ~25 ms) proves it did.
	 */
	{
		unsigned long lv = READ_ONCE(peer->mlo_valid_rx_jiffies);

		after_ms = (lv && time_after(lv, t1_j)) ?
			   (int)jiffies_to_msecs(lv - t1_j) : -1;
	}

	if (!mt7925_mlo_pt_survivor_needs_recovery(after_ms)) {
		mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_RECOVER_STAGE",
				      "not_needed_valid_rx_after_release", sl,
				      old_token, phy->roc_token_id,
				      smconf->mt76.idx, peer, after_ms);
		m->pt_ns[5] = m->pt_ns[1] + (u64)after_ms * NSEC_PER_MSEC;
		rx_ok = true;
	} else {
		/* REPROGRAM_SURVIVOR_CONTEXT then RECOVER_SURVIVOR: bounded
		 * stages, existing lifecycles only. Context first (the ROC
		 * abort detaches DEV/BSS/channel binding, which no earlier
		 * peer-only stage restores), then the peer lifecycle.
		 */
		bool peer_state = false;

		mt7927_mlo_pt_set_state(vif, mvif,
					MT7925_MLO_PT_REPROGRAM_SURVIVOR_CONTEXT,
					"reprogram_survivor_context");
		mt7927_mlo_pt_stamp(mvif, 4);

		for (stage = mt7925_mlo_pt_recover_next(stage, 0);
		     stage == MT7925_MLO_PT_RECOVER_CONTEXT ||
		     stage == MT7925_MLO_PT_RECOVER_RECREATE ||
		     stage == MT7925_MLO_PT_RECOVER_REBUILD;
		     stage = mt7925_mlo_pt_recover_next(stage, rx_ok)) {
			const char *nm = stage == MT7925_MLO_PT_RECOVER_CONTEXT ?
					 "context_dev_bss_bmc_keys" :
					 stage == MT7925_MLO_PT_RECOVER_RECREATE ?
					 "recreate_ptk_authorize" :
					 "rebuild_remove_create_authorize_ptk";

			if (stage != MT7925_MLO_PT_RECOVER_CONTEXT && !peer_state) {
				mt7927_mlo_pt_set_state(vif, mvif,
							MT7925_MLO_PT_RECOVER_SURVIVOR,
							"recover_survivor_peer");
				peer_state = true;
			}

			baseline = peer->mlo_valid_rx_packets;
			mt7927_mlo_surv_trace(vif, mvif,
					      "MLO_SURVIVOR_RECOVER_STAGE", nm,
					      sl, old_token, phy->roc_token_id,
					      smconf->mt76.idx, peer, 0);
			if (stage == MT7925_MLO_PT_RECOVER_CONTEXT) {
				err = mt7927_mlo_pt_reprogram_survivor_context(
					dev, vif, mvif, smconf, link_sta, peer,
					sl, old_token, phy->roc_token_id);
				if (err) {
					/* a failed DEV/BSS/key command: do not
					 * compound it with more commands
					 */
					msta->mlo_link_ready_jiffies[sl] = jiffies;
					mt7927_mlo_pt_failsafe(vif, mvif, phy,
							       "survivor_context_cmd_failed",
							       err);
					return err;
				}
			} else if (stage == MT7925_MLO_PT_RECOVER_RECREATE) {
				err = mt7927_mlo_recreate_authorize_peer(dev, vif, psta,
									 msta, link_sta,
									 peer, sl, false,
									 false);
				mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_PEER",
						      "peer_recreate_ptk_auth", sl,
						      old_token, phy->roc_token_id,
						      smconf->mt76.idx, peer, err);
			} else {
				err = mt7927_mlo_rebuild_peer_assoc_order(dev, vif, psta,
									  link_sta, peer,
									  sl, false);
				mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_PEER",
						      "peer_rebuild_ptk_auth", sl,
						      old_token, phy->roc_token_id,
						      smconf->mt76.idx, peer, err);
			}
			rx_ok = !err && mt7927_mlo_surv_poll_valid_rx(peer, baseline,
								      1500);
			if (rx_ok)
				mt7927_mlo_pt_stamp(mvif, 5);
			mt7927_mlo_surv_trace(vif, mvif,
					      "MLO_SURVIVOR_RECOVER_STAGE",
					      rx_ok ? "stage_rx_recovered" :
					      "stage_no_rx", sl, old_token,
					      phy->roc_token_id,
					      smconf->mt76.idx, peer, err);
		}
	}

	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_WAIT_SURVIVOR_RX,
				"wait_survivor_secure_rx");
	if (!rx_ok) {
		msta->mlo_link_ready_jiffies[sl] = jiffies;
		mt7927_mlo_pt_failsafe(vif, mvif, phy,
				       "survivor_rx_not_recovered", -ENODATA);
		return -ENODATA;
	}

	msta->mlo_link_ready_jiffies[sl] = jiffies;
	mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_RX_READY", "secure_rx", sl,
			      old_token, phy->roc_token_id, smconf->mt76.idx,
			      peer, after_ms);
	return 0;
}

#define MT7927_MLO_SEP18_SETTLE_MS	300

/* Sep-18 replay: after the full-JOIN grant the survivor's slot was released
 * and re-granted by the JOIN itself. If it does not resume security-valid RX
 * on its own, run today's peer lifecycle (recreate, then remove-first
 * rebuild) and require security-valid RX after each stage. Failure is not
 * fatal to the transition: the run characterises both links.
 */
static int mt7927_mlo_sep18_recover_survivor(struct mt792x_dev *dev,
					     struct ieee80211_vif *vif,
					     struct mt792x_vif *mvif,
					     unsigned int sl)
{
	struct mt792x_bss_conf *smconf = mt7927_mlo_resident_bss(dev, mvif, sl);
	struct mt792x_sta *msta = mvif->wep_sta;
	struct ieee80211_sta *psta = msta ?
		wcid_to_sta(&msta->deflink.wcid) : NULL;
	struct mt792x_link_sta *peer = msta ?
		mt7927_mlo_resident_sta(dev, msta, sl) : NULL;
	struct ieee80211_link_sta *link_sta = psta ?
		mt792x_sta_to_link_sta(vif, psta, sl) : NULL;
	enum mt7925_mlo_pt_recover_stage stage = MT7925_MLO_PT_RECOVER_NONE;
	bool resumed, rx_ok = false;
	unsigned int tok = dev->phy.roc_token_id;
	u32 baseline;
	int err = 0;

	if (!smconf || !msta || !psta || !peer || !link_sta) {
		mt7927_mlo_surv_trace(vif, mvif, "MLO_SEP18_SURVIVOR",
				      "no_survivor_objects", sl, 0, tok, -1,
				      peer, -EINVAL);
		return -EINVAL;
	}

	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_RECOVER_SURVIVOR,
				"sep18_recover_survivor");
	mt7927_mlo_pt_stamp(mvif, 4);
	baseline = READ_ONCE(peer->mlo_valid_rx_packets);
	resumed = mt7927_mlo_surv_poll_valid_rx(peer, baseline,
						MT7927_MLO_SEP18_SETTLE_MS);
	mt7927_mlo_surv_trace(vif, mvif, "MLO_SEP18_SURVIVOR",
			      resumed ? "rx_resumed_after_grant" :
			      "no_rx_after_grant", sl, 0, tok,
			      smconf->mt76.idx, peer, 0);
	if (resumed)
		mt7927_mlo_pt_stamp(mvif, 5);
	if (mt7925_mlo_security_rebind) {
		/* The unified both-link rebind runs once the incoming link's
		 * WCID exists (mt7927_mlo_change_sta_links()); a survivor-only
		 * ladder here would run against a partner set that does not
		 * yet contain it.
		 */
		mt7927_mlo_surv_trace(vif, mvif, "MLO_SEP18_SURVIVOR",
				      "deferred_to_security_rebind", sl, 0, tok,
				      smconf->mt76.idx, peer, 0);
		return 0;
	}
	if (!mt7925_mlo_pt_sep18_recovery_wanted(1, resumed))
		return 0;

	/* withhold the survivor from ordinary TX selection until secure RX is
	 * proven again (same gate the destructive migration uses)
	 */
	msta->mlo_link_ready_jiffies[sl] = 0;
	peer->tx_liveness_rx_seen = peer->mlo_valid_rx_packets;
	peer->tx_liveness_tx_seen = peer->wcid.stats.tx_packets;
	peer->tx_liveness_rx_ever_seen = false;
	peer->tx_liveness_rx_progress_jiffies = 0;

	for (stage = mt7925_mlo_pt_peer_recover_next(stage, 0);
	     stage == MT7925_MLO_PT_RECOVER_RECREATE ||
	     stage == MT7925_MLO_PT_RECOVER_REBUILD;
	     stage = mt7925_mlo_pt_peer_recover_next(stage, rx_ok)) {
		bool recreate = stage == MT7925_MLO_PT_RECOVER_RECREATE;

		baseline = READ_ONCE(peer->mlo_valid_rx_packets);
		mt7927_mlo_surv_trace(vif, mvif, "MLO_SEP18_SURVIVOR",
				      recreate ? "stage_recreate_ptk_authorize" :
				      "stage_rebuild_remove_create_authorize_ptk",
				      sl, 0, tok, smconf->mt76.idx, peer, 0);
		err = recreate ?
		      mt7927_mlo_recreate_authorize_peer(dev, vif, psta, msta,
							 link_sta, peer, sl,
							 false, false) :
		      mt7927_mlo_rebuild_peer_assoc_order(dev, vif, psta,
							  link_sta, peer, sl,
							  false);
		mt7927_mlo_surv_trace(vif, mvif, "MLO_SURVIVOR_PEER",
				      recreate ? "peer_recreate_ptk_auth" :
				      "peer_rebuild_ptk_auth", sl, 0, tok,
				      smconf->mt76.idx, peer, err);
		rx_ok = !err && mt7927_mlo_surv_poll_valid_rx(peer, baseline, 1500);
		if (rx_ok)
			mt7927_mlo_pt_stamp(mvif, 5);
		mt7927_mlo_surv_trace(vif, mvif, "MLO_SEP18_SURVIVOR",
				      rx_ok ? "stage_rx_recovered" :
				      "stage_no_rx", sl, 0, tok,
				      smconf->mt76.idx, peer, err);
	}

	msta->mlo_link_ready_jiffies[sl] = jiffies;
	mt7927_mlo_surv_trace(vif, mvif, "MLO_SEP18_SURVIVOR",
			      stage == MT7925_MLO_PT_RECOVER_DONE ?
			      "survivor_recovered" : "survivor_not_recovered",
			      sl, 0, tok, smconf->mt76.idx, peer,
			      stage == MT7925_MLO_PT_RECOVER_DONE ? 0 : -ENODATA);
	return stage == MT7925_MLO_PT_RECOVER_DONE ? 0 : -ENODATA;
}

/* Delta acquisition of the one incoming link (plan kind DELTA_ACQUIRE). */
static int mt7927_mlo_pt_delta_acquire(struct mt792x_dev *dev,
				       struct mt792x_phy *phy,
				       struct ieee80211_vif *vif,
				       struct mt792x_vif *mvif,
				       const struct mt7925_mlo_pt_plan *pt,
				       u16 new_links)
{
	struct mt7927_mlo_state *m = &mvif->mt7927_mlo;
	struct mt792x_bss_conf *sub_mconf;
	int err;

	sub_mconf = mt7927_mlo_resident_bss(dev, mvif, pt->added_link);
	if (!sub_mconf) {
		mt7927_mlo_pt_failsafe(vif, mvif, phy, "no_resident_bss", -EINVAL);
		return -EINVAL;
	}

	m->pt_link = pt->added_link;
	m->pt_req_slot = pt->added_slot;
	/* production: PREPARE_SHADOW_GROUP, the replacement is prepared while
	 * the continuity grant stays held; the survivor-rejoin test gate
	 * reaches here from WAIT_SURVIVOR_RX and keeps its own state
	 */
	mt7927_mlo_pt_set_state(vif, mvif,
				m->pt_state == MT7925_MLO_PT_WAIT_SURVIVOR_RX ?
				MT7925_MLO_PT_ACQUIRE_REPLACEMENT :
				MT7925_MLO_PT_PREPARE_SHADOW_GROUP,
				"delta_acquire");

	mt7927_mlo_pt_stamp(mvif, 6);
	err = mt7925_mlo_roc_delta_acquire(phy, sub_mconf, new_links,
					   pt->added_slot, m->transition_generation);
	if (err) {
		int cont = mt7925_mlo_continuity_get(&phy->mlo_roc);
		bool held = cont >= 0 &&
			    (phy->mlo_roc.held_links &
			     BIT(phy->mlo_roc.grant[cont].link_id));

		/* no fallback of any kind: the continuity grant stays held
		 * and the session keeps running on it
		 */
		if (mt7925_mlo_pt_prepare_reason(err, held) ==
		    MT7925_MLO_PT_REASON_PREPARE_BLOCKED_BY_HELD_GRANT)
			mt7927_mlo_pt_failsafe(vif, mvif, phy,
					       "PREPARE_BLOCKED_BY_HELD_GRANT",
					       err);
		else
			mt7927_mlo_pt_failsafe(vif, mvif, phy,
					       err == -ETIMEDOUT ? "acquire_timeout" :
					       "acquire_rejected", err);
		/* The survivor keeps running, but the dropped link cannot come
		 * back by a delta ACQUIRE: ask for the reassociation that can.
		 */
		mt7925_mlo_recovery_request_reassoc(vif, m->pt_old_links,
						    m->pt_target_links);
		return err;
	}

	mt7927_mlo_pt_stamp(mvif, 7);
	m->pt_token = phy->roc_token_id;
	m->pt_grant_slot = phy->mlo_roc.single_dbdc;
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER,
				"grant_ok");
	return 0;
}

static bool mt7927_mlo_pt_owns_link(struct mt792x_vif *mvif,
				    unsigned int link_id)
{
	return mvif->mt7927_mlo.pt_state == MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER &&
	       mvif->mt7927_mlo.pt_link == (s8)link_id;
}

static void mt7927_mlo_pt_wait_rx(struct ieee80211_vif *vif,
				  struct mt792x_vif *mvif,
				  unsigned int link_id)
{
	if (mt7927_mlo_pt_owns_link(mvif, link_id))
		mt7927_mlo_pt_set_state(vif, mvif,
					MT7925_MLO_PT_WAIT_REPLACEMENT_RX,
					"peer_admitted");
}

/* Security-valid RX on the replacement proves the transition. The pair is
 * committed by the existing steering/validation stage; this only records
 * that the physical acquisition, peer lifecycle and secure RX all held.
 */
static void mt7927_mlo_pt_rx_ready(struct ieee80211_vif *vif,
				   struct mt792x_vif *mvif,
				   unsigned int link_id, u32 valid_rx)
{
	struct mt792x_phy *phy = mvif->phy;
	struct mt792x_bss_conf *sub;

	if (mvif->mt7927_mlo.pt_state != MT7925_MLO_PT_WAIT_REPLACEMENT_RX ||
	    mvif->mt7927_mlo.pt_link != (s8)link_id)
		return;

	mt7927_mlo_pt_stamp(mvif, 8);
	mt7927_mlo_pt_trace(vif, mvif, "MLO_PAIR_TRANSITION_RX_READY",
			    "valid_rx", valid_rx);

	/* VALIDATE_SHADOW_PATH: the replacement incarnation proved secure RX */
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_VALIDATE_SHADOW_PATH,
				"replacement_secure_rx");
	sub = mt792x_vif_to_link(mvif, link_id);
	if (sub)
		mt7925_mlo_grant_validate(&phy->mlo_roc, mvif->mt7927_mlo.pt_token,
					  sub->mt76.idx, 1);

	/* HANDOFF_CONTINUITY: only a VALIDATED incarnation may take it over.
	 * In the delta shape the survivor stays in the final pair, so its
	 * grant is kept (HELD), not made obsolete.
	 */
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_HANDOFF_CONTINUITY,
				"handoff");
	if (!sub || mt7925_mlo_continuity_handoff(&phy->mlo_roc,
						  mvif->mt7927_mlo.pt_token,
						  sub->mt76.idx, 1) < 0) {
		mt7927_mlo_pt_failsafe(vif, mvif, phy, "handoff_refused",
				       -EPERM);
		return;
	}

	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_MIGRATE_TRAFFIC,
				"continuity_handed_off");
	/* RELEASE_OLD_GRANT: only incarnations made OBSOLETE by the handoff
	 * (none in the delta shape; the removed link's slot went at the
	 * shrink), each still through the continuity guard
	 */
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_RELEASE_OLD_GRANT,
				"release_obsolete");
	mt7925_mlo_roc_release_obsolete(phy, mvif);
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_CONVERGED, "converged");
	mt7927_mlo_pt_stamp(mvif, 9);
	mt7927_mlo_pt_trace(vif, mvif, "MLO_PAIR_TRANSITION_COMMIT",
			    "commit", 0);
	mt7927_mlo_pt_timeline(vif, mvif, "commit");
	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_IDLE, "done");
}

/* Replacement peer exists but no security-valid RX yet: the survivor stays
 * as it is and the transition is not reported complete. Returns -ENODATA when
 * this replacement generation failed, so the caller does not publish the link.
 */
static int mt7927_mlo_pt_rx_pending(struct ieee80211_vif *vif,
				    struct mt792x_vif *mvif,
				    struct mt792x_dev *dev,
				    unsigned int link_id)
{
	struct mt792x_bss_conf *sub;

	if (mvif->mt7927_mlo.pt_state != MT7925_MLO_PT_WAIT_REPLACEMENT_RX ||
	    mvif->mt7927_mlo.pt_link != (s8)link_id)
		return 0;

	/* not validated: no handoff, the continuity grant stays protected */
	sub = mt792x_vif_to_link(mvif, link_id);
	if (sub)
		mt7925_mlo_grant_validate(&dev->phy.mlo_roc,
					  mvif->mt7927_mlo.pt_token,
					  sub->mt76.idx, 0);
	mt7927_mlo_pt_failsafe(vif, mvif, &dev->phy, "no_secure_rx", -ENODATA);
	return -ENODATA;
}

static int mt7927_mlo_single_acquire_probe(struct mt792x_dev *dev,
					   struct mt792x_phy *phy,
					   struct mt792x_bss_conf *sub_mconf,
					   u16 sel_links)
{
	struct mt7925_mlo_roc_state *st = &phy->mlo_roc;
	unsigned int held_tok0 = st->held_token, held_links0 = st->held_links;
	unsigned long t0;
	u8 token;
	int ret, w;

	if (test_and_set_bit(MT76_STATE_ROC, &phy->mt76->state)) {
		printk(KERN_INFO "MLO_ROC_SINGLE_PRE: INVALID roc_busy\n");
		return -EBUSY;
	}


	printk(KERN_INFO "MLO_ROC_SINGLE_PRE: held_token=%u held_links=0x%x pending_token=%u pending_links=0x%x timedout_token=%u orphan_bss=0x%x last_token=%u sub_link=%u sub_bss=%u sel_links=0x%x\n",
	       st->held_token, st->held_links, st->pending_token,
	       st->pending_links, st->timedout_token, st->orphan_bss,
	       phy->roc_token_id, sub_mconf->link_id, sub_mconf->mt76.idx,
	       sel_links);

	token = ++phy->roc_token_id;
	st->single_status = st->single_dbdc = st->single_ch = 0;
	st->single_evt_ms = 0;
	st->single_bss = sub_mconf->mt76.idx;
	st->single_t0 = jiffies;
	st->single_token = token;
	st->single_state = 1;
	t0 = jiffies;

	ret = mt7925_mcu_set_mlo_single_acquire(phy, sub_mconf, sel_links, 5,
						token, -1);
	printk(KERN_INFO "MLO_ROC_SINGLE_SENT: token=%u fresh=%d mcu_ret=%d pending_full_join=%u\n",
	       token, token != held_tok0, ret, st->pending_links);

	if (ret >= 0) {
		w = wait_event_timeout(phy->roc_wait,
				       READ_ONCE(st->single_state) >= 2,
				       msecs_to_jiffies(2500));
		printk(KERN_INFO "MLO_ROC_SINGLE_RESULT: token=%u state=%u event=%s dt_ms=%u status=%u dbdcband=%u ch=%u waited_ms=%u\n",
		       token, st->single_state,
		       w ? (st->single_state == 2 ? "GRANT" :
			    st->single_state == 3 ? "SUB_LINK" : "FAIL_STATUS") :
			   "NONE",
		       st->single_evt_ms, st->single_status, st->single_dbdc,
		       st->single_ch, jiffies_to_msecs(jiffies - t0));
	}

	/* Always cancel only the probe's own token (never the held one). */
	if (mt7925_mlo_roc_abort_guard(phy, sub_mconf->vif, token,
				       sub_mconf->link_id, sub_mconf->mt76.idx,
				       "single_acquire_probe"))
		mt7925_mcu_abort_mlo_roc(phy, sub_mconf, token);
	printk(KERN_INFO "MLO_ROC_SINGLE_CLEANUP: aborted token=%u bss=%u; held_token=%u held_links=0x%x (before %u/0x%x) pending_links=0x%x\n",
	       token, sub_mconf->mt76.idx, st->held_token, st->held_links,
	       held_tok0, held_links0, st->pending_links);

	clear_bit(MT76_STATE_ROC, &phy->mt76->state);
	return -ECANCELED;
}

/* PRESERVE_CONTINUITY_GRANT: mark the held slot of @link_id (the survivor)
 * as the continuity grant when it carries security-valid RX. Pure
 * bookkeeping; from here on the continuity guard refuses every abort of
 * that (token, bss_idx) until another incarnation proves secure RX.
 * Returns 0 when a security-valid continuity grant is marked.
 */
static int mt7927_mlo_mark_continuity(struct mt792x_dev *dev,
				      struct mt792x_phy *phy,
				      struct ieee80211_vif *vif,
				      struct mt792x_vif *mvif,
				      unsigned int link_id, const char *why)
{
	struct mt7925_mlo_roc_state *st = &phy->mlo_roc;
	struct mt792x_bss_conf *mconf = mt7927_mlo_resident_bss(dev, mvif, link_id);
	struct mt792x_link_sta *peer = mvif->wep_sta ?
		mt7927_mlo_resident_sta(dev, mvif->wep_sta, link_id) : NULL;
	struct ieee80211_bss_conf *lc = mt792x_vif_to_bss_conf(vif, link_id);
	unsigned long lv = peer ? READ_ONCE(peer->mlo_valid_rx_jiffies) : 0;
	unsigned int token = 0, ch = 0, age = UINT_MAX;
	int secure = 0, idx = -1;

	if (mconf && (st->held_links & BIT(link_id))) {
		token = mt7925_mlo_roc_state_owner_token(st, link_id);
		if (lv)
			age = jiffies_to_msecs(jiffies - lv);
		secure = mt7925_mlo_pt_secure_rx_fresh(!!lv, age);
		if (lc && lc->chanreq.oper.chan)
			ch = lc->chanreq.oper.chan->hw_value;
		idx = mt7925_mlo_continuity_mark(st, token, link_id,
						 mconf->mt76.idx,
						 mt7925_mlo_band_slot(
							 mt7927_mlo_link_band(vif,
									      link_id)),
						 ch, secure);
	}

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_CONTINUITY_MARK: id=%u %s link=%u token=%u bss_idx=%d ch=%u secure_rx=%d valid_rx_age_ms=%d held_links=0x%x active_links=0x%x valid_links=0x%x idx=%d\n",
		       mvif->mt7927_mlo.transition_generation, why, link_id,
		       token, mconf ? (int)mconf->mt76.idx : -1, ch, secure,
		       age == UINT_MAX ? -1 : (int)age, st->held_links,
		       vif->active_links, vif->valid_links, idx);

	if (idx >= 0 && secure)
		printk(KERN_INFO
		       "MLO_CONTINUITY_OWNER_ARMED: id=%u token=%u bss=%d link=%u secure_rx=1 valid_rx_age_ms=%d idx=%d\n",
		       mvif->mt7927_mlo.transition_generation, token,
		       mconf ? (int)mconf->mt76.idx : -1, link_id,
		       age == UINT_MAX ? -1 : (int)age, idx);

	if (idx < 0)
		return -ENOENT;
	return secure ? 0 : -ENODATA;
}

int mt7927_mlo_change_vif_links(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				u16 old_links, u16 new_links,
				struct ieee80211_bss_conf *old[IEEE80211_MLD_MAX_NUM_LINKS])
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long add = new_links & ~old_links;
	unsigned long rem = old_links & ~new_links;
	unsigned long created = 0;
	unsigned int link_id;
	u64 sep18_abort_ns = 0;
	unsigned int blocked0;
	int cont_err = -ENOENT;
	int err = 0;

	if (!mt7927_mlo_active_transition(dev, vif, old_links, new_links))
		return -EOPNOTSUPP;

	mt792x_mutex_acquire(dev);

	mvif->mt7927_mlo.transition_generation++;
	mvif->mt7927_mlo.negotiated_links = vif->valid_links;
	if (hweight16(new_links) == 1) {
		mvif->mt7927_mlo.migration_from_links = old_links;
		mvif->mt7927_mlo.migration_to_links = 0;
	} else if (hweight16(old_links) == 1 && hweight16(new_links) == 2) {
		mvif->mt7927_mlo.migration_to_links = new_links;
	}

	/* Backfill resident_links for any negotiated link whose real mconf/
	 * mlink objects already exist but were never noted resident. This
	 * happens for links admitted by the generic (non-MT7927-specific)
	 * path during INITIAL association -- mt7925_change_vif_links()'s
	 * single-link-creation branch and mt7925_mac_sta_add_links() build
	 * real objects in mvif->link_conf[]/sta.link[] without ever calling
	 * mt7927_mlo_note_bss()/note_sta(), so resident_links previously
	 * stayed 0 for such a link until its first MT7927-dispatched
	 * transition -- proven live: negotiated_links correctly showed a
	 * link while resident_links did not, for two different negotiation
	 * orderings, before this fix. Since this function runs on every
	 * old&&new-nonzero call including the ones inside the initial
	 * negotiation dance itself (0x0->0x4->...->settled), this backfill
	 * completes before auto-steering's first tick ever runs -- fixing
	 * the model at its actual initialization point instead of having
	 * the scorer permanently ignore resident_links.
	 */
	{
		unsigned long to_check = vif->valid_links &
					 ~mvif->mt7927_mlo.resident_links;
		unsigned int lid;

		for_each_set_bit(lid, &to_check, IEEE80211_MLD_MAX_NUM_LINKS) {
			if (mt7927_mlo_resident_bss(dev, mvif, lid) &&
			    mt7927_mlo_resident_sta(dev, &mvif->sta, lid)) {
				mvif->mt7927_mlo.resident_links |= BIT(lid);
				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "MLO_RESIDENT_TRACE: fn=backfill link=%u negotiated_links=0x%x resident_links=0x%x\n",
					      lid, mvif->mt7927_mlo.negotiated_links,
					      mvif->mt7927_mlo.resident_links);
			}
		}
	}

	/* Make-before-break: mark the security-valid survivor grant as the
	 * continuity grant BEFORE anything is released, so no release below
	 * (selective, legacy release-all, stale, orphan) can abort it.
	 */
	if (rem && hweight16(new_links) == 1)
		cont_err = mt7927_mlo_mark_continuity(dev, phy, vif, mvif,
						      __ffs(new_links), "shrink");
	blocked0 = phy->mlo_roc.abort_blocked;

	if (rem && (mt7925_mlo_roc_release & 2)) {
		mt7925_mlo_roc_release_transition(phy, mvif, old_links,
						  new_links, vif->valid_links);
		sep18_abort_ns = ktime_get_ns();
	}

	/* mac80211's _ieee80211_set_active_links() unconditionally shrinks to
	 * old_active & active_links before growing to the final mask
	 * whenever the two masks overlap (net/mac80211/link.c:650-659,
	 * "if there's at least one link that stays active across the change
	 * then switch to it (to those) first, and then enable the
	 * additional links"). This is mandatory core behaviour, not a
	 * test-only shape: every ordinary 1-out/1-in pair swap therefore
	 * reaches this callback TWICE -- first a rem-only call whose
	 * new_links is the single surviving link, then an add-only call
	 * that grows back to the final two-link pair. Proven live: old=0x6
	 * new=0x2 (rem=0x4) immediately followed by old=0x2 new=0x3
	 * (add=0x1) in the same activate_links_work() run.
	 *
	 * A single combined callback with hweight(old)==2 && hweight(new)==2
	 * in one call never happens through this path -- mac80211 only takes
	 * that shape when old and new share no link at all, which is not a
	 * swap. Handling it here was dead code; the two real phases below
	 * replace it.
	 */

	/* Phase 1: shrink to the mandatory single survivor. new_links holds
	 * exactly one bit here, so mt7927_mlo_reassign_active_bands() cannot
	 * be called (it hard-requires two links) and
	 * __ffs(new_links & ~BIT(primary)) would be undefined (no second
	 * bit to find) -- use an explicit sentinel instead. Retire only the
	 * departing link; the survivor's own resident BSS/WCID/key/band
	 * state is untouched.
	 */
	if (hweight16(new_links) == 1) {
		mvif->mt7927_mlo.primary_link_id = __ffs(new_links);
		mvif->mt7927_mlo.secondary_link_id = IEEE80211_LINK_UNSPECIFIED;

		mt7927_mlo_pt_begin(vif, mvif, old_links, new_links);
		mt7927_mlo_pt_set_state(vif, mvif,
					MT7925_MLO_PT_PRESERVE_CONTINUITY_GRANT,
					cont_err ? "no_secure_continuity_grant" :
						   "continuity_grant_marked");
		mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_QUIESCE_REMOVED,
					"quiesce_removed");

		if (mt7925_mlo_sep18_replay && sep18_abort_ns) {
			mvif->mt7927_mlo.pt_ns[1] = sep18_abort_ns;
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_SEP18_CFG: roc_release=%u delta_acquire=%d survivor_rejoin=%d old=0x%x new=0x%x abort_mono_us=%llu held_after=0x%x held_token=%u\n",
				       mt7925_mlo_roc_release,
				       mt7925_mlo_delta_acquire,
				       mt7925_mlo_survivor_rejoin, old_links,
				       new_links,
				       (unsigned long long)(sep18_abort_ns / 1000),
				       phy->mlo_roc.held_links,
				       phy->mlo_roc.held_token);
		}

		for_each_set_bit(link_id, &rem, IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_bss_conf *mconf;
			struct mt792x_link_sta *mlink;

			mconf = mt7927_mlo_resident_bss(dev, mvif, link_id);
			mlink = mt7927_mlo_resident_sta(dev, &mvif->sta, link_id);
			if (!mconf || !mlink)
				continue;

			mvif->mt7927_mlo.link[link_id].removing = true;
			mt7927_mlo_note_bss(mvif, mconf, mlink, false);
			mvif->mt7927_mlo.link[link_id].removing = false;
			mvif->mt7927_mlo.link[link_id].roc_admitted = false;
		}

		mvif->mt7927_mlo.traffic_active_links = new_links;

		/* a release that would have aborted the continuity grant was
		 * refused: the survivor keeps its slot; fail this transition
		 * safely on it (the grow phase starts a fresh one)
		 */
		if (phy->mlo_roc.abort_blocked != blocked0) {
			mt7927_mlo_pt_failsafe(vif, mvif, phy,
					       "continuity_abort_blocked", -EPERM);
			mt792x_mutex_release(dev);
			return 0;
		}

		mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_PRESERVE_SURVIVOR,
					"survivor_preserved");
		mt792x_mutex_release(dev);
		return 0;
	}

	/* Phase 2: grow from the lone survivor to the final two-link pair.
	 * rem is empty by construction (the one bit in old_links is always
	 * the survivor, which stays in new_links). Admit the substitute,
	 * band the final pair, and commit pair bookkeeping only once ROC
	 * admission has actually succeeded -- a failure here must never
	 * leave mac80211 and the driver disagreeing about which pair is
	 * active.
	 */
	if (hweight16(old_links) == 1 && hweight16(new_links) == 2 &&
	    !rem && hweight16(add) == 1) {
		unsigned int anchor_id = __ffs(old_links);
		unsigned int sub_id = __ffs(add);
		struct mt792x_bss_conf *anchor_mconf;
		struct mt792x_link_sta *anchor_mlink;
		struct mt792x_link_sta *sub_mlink;
		bool real_grant = false;
		bool skip_legacy_join = false;

		if (mvif->mt7927_mlo.pt_state != MT7925_MLO_PT_PRESERVE_SURVIVOR ||
		    mvif->mt7927_mlo.pt_survivor != old_links) {
			mt7927_mlo_pt_begin(vif, mvif, old_links, new_links);
			cont_err = mt7927_mlo_mark_continuity(dev, phy, vif, mvif,
							      anchor_id, "grow");
			mt7927_mlo_pt_set_state(vif, mvif,
						MT7925_MLO_PT_PRESERVE_CONTINUITY_GRANT,
						cont_err ? "no_secure_continuity_grant" :
							   "continuity_grant_marked");
			mt7927_mlo_pt_set_state(vif, mvif,
						MT7925_MLO_PT_PRESERVE_SURVIVOR,
						"no_shrink_phase");
		}
		mvif->mt7927_mlo.pt_target_links = new_links;
		mvif->mt7927_mlo.pt_added = add;

		/* The ROC ACQUIRE/anchor slot must be the resident survivor --
		 * the link firmware already has a live JOIN history for --
		 * never &mvif->bss_conf directly. bss_conf.link_id is the
		 * vif's ORIGINAL deflink from true initial association and is
		 * never reassigned on this path; once the shrink phase above
		 * retires that original deflink, mt7925_mcu_set_mlo_roc()'s
		 * own stale-deflink fallback (__ffs(sel_links)) would
		 * silently anchor on the brand-new substitute instead --
		 * proven live as total firmware silence (zero ROCEV of any
		 * kind across the full 5s wait), not a rejection. Resolve the
		 * real survivor explicitly; never relabel mvif->bss_conf
		 * itself to work around this -- its identity (BSS index,
		 * OMAC, key state, RSSI) is used elsewhere.
		 */
		anchor_mconf = mt7927_mlo_resident_bss(dev, mvif, anchor_id);
		anchor_mlink = mt7927_mlo_resident_sta(dev, &mvif->sta, anchor_id);
		if (!anchor_mconf || !anchor_mlink || !anchor_mlink->wcid.link_valid) {
			err = -EINVAL;
			goto err_remove_created;
		}

		err = mt7927_mlo_add_bss_link(dev, vif, mvif, sub_id, &created);
		if (err)
			goto err_remove_created;

		/* mt7927_mlo_add_bss_link() marks the substitute traffic-
		 * eligible immediately on success -- correct for the generic
		 * path below, wrong here: the substitute must stay dormant
		 * until ROC actually grants, not merely until the error is
		 * swallowed.
		 */
		sub_mlink = mt7927_mlo_resident_sta(dev, &mvif->sta, sub_id);
		if (sub_mlink)
			sub_mlink->wcid.link_valid = false;
		mvif->mt7927_mlo.traffic_active_links &= ~BIT(sub_id);
		mvif->mt7927_mlo.link[sub_id].roc_admitted = false;

		err = mt7927_mlo_reassign_active_bands(dev, vif, mvif,
						       new_links, add & ~created);
		if (err)
			goto err_remove_created;

		/* DEV/BSS band identity and the peer STA_REC form one firmware
		 * dependency chain. Rebind the resident peer before requesting ROC;
		 * leaving STA_REC on the old PHY slot makes a later repeated JOIN
		 * silently time out even though DEV/BSS updates were acknowledged.
		 */
		if (mvif->wep_sta) {
			struct ieee80211_sta *psta =
				wcid_to_sta(&mvif->wep_sta->deflink.wcid);

			if (!psta) {
				err = -EINVAL;
				goto err_remove_created;
			}

			err = mt7927_mlo_refresh_reassigned_sta(dev, vif, psta,
							 mvif->wep_sta, new_links,
							 add && is_mt7927(&dev->mt76) &&
							 mt7925_defer_primary_authorize_active());
			if (err)
				goto err_remove_created;
		}

		if (READ_ONCE(mt7925_test_mt7927_sublink_postkey) &&
		    READ_ONCE(mt7925_test_mt7927_sublink_roc) && is_mt7927(&dev->mt76)) {
			/* Never attempt ROC from here: mac80211's
			 * ieee80211_key_switch_links() (uncontrolled, not
			 * exported, proven by source trace to run strictly
			 * after this callback returns) is what actually
			 * enables the substitute's group key. Record pending
			 * state on the PEER (not mvif -- mt792x_vif caches no
			 * lifetime-safe peer pointer) and return; the SET_KEY
			 * trigger in mt7925_set_key() arms the real work once
			 * that key install actually happens. RCU is held only
			 * long enough to resolve the peer and write five plain
			 * fields -- no blocking call inside the section.
			 */
			struct ieee80211_sta *psta;
			struct mt792x_sta *pmsta = NULL;

			rcu_read_lock();
			psta = ieee80211_find_sta(vif, vif->cfg.ap_addr);
			if (psta)
				pmsta = (struct mt792x_sta *)psta->drv_priv;
			if (pmsta) {
				if (pmsta->postkey_pending)
					mt7927_mlo_pending_clear(pmsta);

				pmsta->postkey_pending = true;
				pmsta->postkey_sublink_id = sub_id;
				pmsta->postkey_survivor_id = anchor_id;
				pmsta->postkey_generation =
					mvif->mt7927_mlo.transition_generation;
				pmsta->postkey_roc_attempted = false;
			}
			rcu_read_unlock();

			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "POSTKEY_PENDING_SET: substitute=%u survivor=%u gen=%u found_peer=%d\n",
				      sub_id, anchor_id,
				      mvif->mt7927_mlo.transition_generation,
				      !!pmsta);

			mt792x_mutex_release(dev);
			return 0;
		}

		if (READ_ONCE(mt7925_test_mt7927_sublink_roc) && is_mt7927(&dev->mt76)) {
			/* Experimental: standalone SUB_LINK ROC for the
			 * substitute only, instead of re-requesting the full
			 * pair. Survivor's own resident BSS/WCID/key/band
			 * state (anchor_mconf/anchor_mlink) is never touched
			 * in this branch -- no abort, deactivate, reassign, or
			 * refresh runs against it here. This answers whether
			 * firmware supports admitting only the dormant
			 * substitute while the survivor stays untouched,
			 * independent of whatever caused the full-pair JOIN
			 * (anchor=survivor) to receive zero response.
			 */
			struct ieee80211_bss_conf *sub_link_conf =
				mt792x_vif_to_bss_conf(vif, sub_id);
			struct mt792x_bss_conf *sub_roc_mconf =
				mt7927_mlo_resident_bss(dev, mvif, sub_id);

			if (!sub_roc_mconf || !sub_link_conf ||
			    !sub_link_conf->chanreq.oper.chan) {
				err = -EINVAL;
				goto err_remove_created;
			}

			if (READ_ONCE(mt7925_test_mt7927_sublink_prekey)) {
				/* Isolate exactly one new variable on top of
				 * the sublink-ROC experiment: rebind the
				 * substitute's BSS/BMC group keys using only
				 * BSS/BMC state (mt7927_mlo_add_bss_link()
				 * already created it above) and mac80211's own
				 * cached keys -- no peer STA_REC, no
				 * wcid.link_valid touch. A prekey failure must
				 * prevent ROC entirely rather than proceed on
				 * an unconfirmed key state.
				 */
				struct mt7925_group_key_rebind_result prekey_result = {};
				int prekey_err;

				prekey_err = mt7925_mlo_group_key_rebind(dev, vif,
									 sub_id,
									 &prekey_result);

				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "MLO_SUBLINK_PREKEY_TRACE: link=%u bss_idx=%u keys_seen=%u already_current=%u installs_attempted=%u installs_succeeded=%u result=%d\n",
					      sub_id, sub_roc_mconf->mt76.idx,
					      prekey_result.keys_seen,
					      prekey_result.already_current,
					      prekey_result.installs_attempted,
					      prekey_result.installs_succeeded,
					      prekey_err);

				if (prekey_err) {
					real_grant = false;
					goto sublink_prekey_failed;
				}
			}

			err = mt7925_set_sublink_roc(phy, sub_roc_mconf,
						     sub_link_conf->chanreq.oper.chan,
						     sub_roc_mconf->mt76.band_idx);
			real_grant = (err == 0);
sublink_prekey_failed:

			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_SUBLINK_ROC_ANCHOR_TRACE: old_links=0x%x new_links=0x%x survivor=0x%x add=0x%lx anchor_link=%u anchor_bss_idx=%u sublink_link=%u sublink_bss_idx=%u result=%d real_grant=%d\n",
				      old_links, new_links, old_links, add,
				      anchor_id, anchor_mconf->mt76.idx, sub_id,
				      sub_roc_mconf->mt76.idx, err, real_grant);

			/* Never propagate a sublink ROC failure as a hard
			 * error -- same tolerance as the default path's
			 * noroc_admit bypass: the substitute just stays
			 * resident and dormant through the shared !real_grant
			 * handling below, and this run must not silently fall
			 * back to the full-pair path (that would contaminate
			 * which mechanism actually produced the result).
			 */
			err = 0;
		} else if ((READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)) &&
			   mt7927_mlo_join_retry_allowed(dev, vif) && (created & add)) {
			printk(KERN_INFO
			      "MLO_ROC_RETRY: attempt=1/2 add=0x%lx created=0x%lx sel_links=0x%x\n",
			      add, created, new_links);
		}

		if (READ_ONCE(mt7925_test_mt7927_roc_single_acquire) &&
		    !READ_ONCE(mt7925_test_mt7927_sublink_roc) &&
		    is_mt7927(&dev->mt76) && (created & add)) {
			struct mt792x_bss_conf *sub_probe_mconf =
				mt7927_mlo_resident_bss(dev, mvif, sub_id);

			err = sub_probe_mconf ?
			      mt7927_mlo_single_acquire_probe(dev, phy,
							      sub_probe_mconf,
							      new_links) :
			      -EINVAL;
			goto err_remove_created;
		}

		if (!mt7925_mlo_sep18_replay && is_mt7927(&dev->mt76) &&
		    !READ_ONCE(mt7925_test_mt7927_sublink_roc)) {
			struct mt7925_mlo_pt_plan pt;
			int bands[3];
			unsigned int lid;

			for (lid = 0; lid < 3; lid++)
				bands[lid] = mt7927_mlo_link_band(vif, lid);
			mt7925_mlo_pt_plan(old_links, new_links, vif->valid_links,
					   phy->mlo_roc.held_links, bands, &pt);
			mvif->mt7927_mlo.pt_kind = pt.kind;

			if (pt.kind == MT7925_MLO_PT_ALREADY_HELD) {
				/* 3-slot association-time JOIN: the incoming
				 * link's slot is already granted and held.
				 * Re-requesting it is never needed; admit the
				 * peer on the standing grant.
				 */
				mvif->mt7927_mlo.pt_link = pt.added_link;
				mvif->mt7927_mlo.pt_req_slot = pt.added_slot;
				/* bind the standing grant's owner token so the
				 * RX-ready handoff validates the held (token,
				 * bss_idx), not the zero pt_begin() reset value
				 */
				mvif->mt7927_mlo.pt_token =
					mt7925_mlo_roc_state_owner_token(&phy->mlo_roc,
									 pt.added_link);
				mvif->mt7927_mlo.pt_grant_slot = pt.added_slot;
				mt7927_mlo_pt_set_state(vif, mvif,
							MT7925_MLO_PT_PREPARE_SHADOW_GROUP,
							"already_held");
				mt7927_mlo_pt_set_state(vif, mvif,
							MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER,
							"already_held_grant");
				real_grant = true;
				skip_legacy_join = true;
			} else if (mt7925_mlo_delta_acquire &&
				   pt.kind == MT7925_MLO_PT_DELTA_ACQUIRE) {
				if (mt7925_mlo_pt_survivor_handoff_wanted(pt.kind,
						mt7925_mlo_survivor_rejoin,
						pt.held_survivor)) {
					err = mt7927_mlo_pt_survivor_handoff(dev, phy,
									     vif, mvif,
									     &pt);
					if (err)
						goto err_remove_created;
				}
				err = mt7927_mlo_pt_delta_acquire(dev, phy, vif, mvif,
								  &pt, new_links);
				if (err)
					goto err_remove_created;
				real_grant = true;
				skip_legacy_join = true;
			} else if (mt7925_mlo_delta_acquire &&
				   pt.kind == MT7925_MLO_PT_HIGHBAND_SHARED_SLOT) {
				/* The added link needs the survivor's own
				 * physical slot (5 <-> 6 on slot 1). That is a
				 * firmware high-band retune (MLSR switch, UNI
				 * 0x59), not a second acquisition; its mode
				 * values are not recovered, so stop safely on
				 * the survivor instead of sending a request
				 * firmware is known not to answer.
				 */
				mvif->mt7927_mlo.pt_link = pt.added_link;
				mvif->mt7927_mlo.pt_req_slot = pt.added_slot;
				mt7927_mlo_pt_failsafe(vif, mvif, phy,
						       "MLSR_HIGH_BAND_SWITCH_REQUIRED",
						       -EOPNOTSUPP);
				err = -EOPNOTSUPP;
				goto err_remove_created;
			}
		}

		if (!skip_legacy_join &&
		    (!READ_ONCE(mt7925_test_mt7927_sublink_roc) || !is_mt7927(&dev->mt76))) {
			if (mt7925_mlo_sep18_replay && is_mt7927(&dev->mt76)) {
				mt7927_mlo_pt_set_state(vif, mvif,
							MT7925_MLO_PT_ACQUIRE_REPLACEMENT,
							"sep18_full_join");
				mt7927_mlo_pt_stamp(mvif, 2);
				mt7927_mlo_surv_trace(vif, mvif,
						      "MLO_SEP18_JOIN_SEND",
						      "full_two_tlv_join",
						      anchor_id, 0,
						      phy->roc_token_id + 1,
						      anchor_mconf->mt76.idx,
						      anchor_mlink, 0);
			}
			err = mt7925_set_mlo_roc(phy, anchor_mconf, new_links);
			if (!err)
				real_grant = true;
			if (mt7925_mlo_sep18_replay && is_mt7927(&dev->mt76)) {
				mt7927_mlo_pt_stamp(mvif, 3);
				mt7927_mlo_surv_trace(vif, mvif,
						      "MLO_SEP18_JOIN_RESULT",
						      err ? "no_grant" : "grant",
						      anchor_id, 0,
						      phy->roc_token_id,
						      anchor_mconf->mt76.idx,
						      anchor_mlink, err);
			}

			if (err == -ETIMEDOUT && (created & add) &&
			    mt7927_mlo_join_retry_allowed(dev, vif)) {
				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "MLO_ROC_RETRY: attempt=2/2 add=0x%lx created=0x%lx sel_links=0x%x\n",
					      add, created, new_links);
				err = mt7925_set_mlo_roc(phy, anchor_mconf, new_links);
				if (!err)
					real_grant = true;
				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "MLO_ROC_RETRY: attempt=2/2 result=%d\n", err);
			}

			if (err == -ETIMEDOUT && !(created & add)) {
				dev_warn(dev->mt76.dev,
					 "MT7927 MLO: resident link re-entry continuing after ROC timeout links=0x%x add=0x%lx\n",
					 new_links, add);
				err = 0;
			} else if (err == -ETIMEDOUT && (created & add) &&
				   READ_ONCE(mt7925_test_mt7927_mlo_noroc_admit)) {
				/* Diagnostic bypass of the ERROR only -- swallow
				 * -ETIMEDOUT so mac80211 isn't handed a failure, but
				 * never mark the ungranted substitute eligible or
				 * commit it into the active pair. noroc_admit measures
				 * what happens without a real grant; it must not fake
				 * one.
				 */
				dev_warn(dev->mt76.dev,
					 "MUP: noroc_admit bypassing ROC failure for link_id=%u (kept dormant)\n",
					 sub_id);
				err = 0;
			} else if (err) {
				goto err_remove_created;
			}
		}

		if (mt7925_mlo_sep18_replay && mt7925_mlo_security_rebind &&
		    is_mt7927(&dev->mt76) && !skip_legacy_join && real_grant) {
			/* The old privilege was released and the full JOIN
			 * re-granted the group: every active link now has a
			 * new firmware incarnation, survivor included.
			 */
			mvif->mt7927_mlo.radio_group_reincarnated = true;
			mvif->mt7927_mlo.rebind_target_links = new_links;
		}

		if (mt7925_mlo_sep18_replay && is_mt7927(&dev->mt76) &&
		    !skip_legacy_join && real_grant) {
			int serr = mt7927_mlo_sep18_recover_survivor(dev, vif,
								     mvif,
								     anchor_id);

			mt7927_mlo_pt_timeline(vif, mvif, "sep18_survivor_done");
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_SEP18_DONE: survivor_recovery=%d anchor_link=%u sub_link=%u active_links=0x%x valid_links=0x%x\n",
				       serr, anchor_id, sub_id, vif->active_links,
				       vif->valid_links);
		}

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)) {
			struct mt792x_bss_conf *sub_mconf =
				mt7927_mlo_resident_bss(dev, mvif, sub_id);

			printk(KERN_INFO
			      "MLO_ROC_ANCHOR_TRACE: old_links=0x%x new_links=0x%x survivor=0x%x add=0x%lx embedded_deflink_id=%u anchor_link=%u anchor_bss_idx=%u sublink_link=%u sublink_bss_idx=%u token=%u result=%d real_grant=%d\n",
			      old_links, new_links, old_links, add,
			      mvif->bss_conf.link_id, anchor_id,
			      anchor_mconf->mt76.idx, sub_id,
			      sub_mconf ? sub_mconf->mt76.idx : 0xff,
			      phy->roc_token_id, err, real_grant);
		}

		if (!real_grant) {
			/* No real grant: preserve survivor-only operation.
			 * The substitute stays resident (its `created` bit is
			 * not unwound, so a later retry can reuse it) but
			 * ineligible for traffic -- do not commit it into the
			 * active pair.
			 */
			mt792x_mutex_release(dev);
			return 0;
		}

		/* Substitute is genuinely admitted and banded -- commit the
		 * completed pair only now that a real grant occurred.
		 * roc_admitted is stamped with the CURRENT transition
		 * generation (bumped once per change_vif_links() call, at
		 * entry) so mt7927_mlo_change_sta_links() -- which runs next,
		 * in the same mac80211 callback sequence -- can prove this
		 * grant is fresh rather than a stale event left over from an
		 * earlier retirement of this same link_id.
		 */
		if (sub_mlink)
			sub_mlink->wcid.link_valid = true;
		mvif->mt7927_mlo.traffic_active_links = new_links;
		mvif->mt7927_mlo.primary_link_id = anchor_id;
		mvif->mt7927_mlo.secondary_link_id = sub_id;
		mvif->mt7927_mlo.link[sub_id].roc_admitted = true;
		mvif->mt7927_mlo.link[sub_id].admitted_generation =
			mvif->mt7927_mlo.transition_generation;

		mt792x_mutex_release(dev);
		return 0;
	}

	mvif->mt7927_mlo.traffic_active_links = new_links;
	mvif->mt7927_mlo.primary_link_id = __ffs(new_links);
	mvif->mt7927_mlo.secondary_link_id =
		__ffs(new_links & ~BIT(mvif->mt7927_mlo.primary_link_id));

	for_each_set_bit(link_id, &rem, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *mconf;
		struct mt792x_link_sta *mlink;

		mconf = mt7927_mlo_resident_bss(dev, mvif, link_id);
		mlink = mt7927_mlo_resident_sta(dev, &mvif->sta, link_id);
		if (!mconf || !mlink)
			continue;

		mvif->mt7927_mlo.link[link_id].removing = true;
		mt7927_mlo_note_bss(mvif, mconf, mlink, false);
		mvif->mt7927_mlo.link[link_id].removing = false;
		mvif->mt7927_mlo.link[link_id].roc_admitted = false;
	}

	for_each_set_bit(link_id, &add, IEEE80211_MLD_MAX_NUM_LINKS) {
		err = mt7927_mlo_add_bss_link(dev, vif, mvif, link_id, &created);
		if (err)
			goto err_remove_created;
	}

	err = mt7927_mlo_reassign_active_bands(dev, vif, mvif, new_links,
					     add & ~created);
	if (err)
		goto err_remove_created;

	if (add && READ_ONCE(mt7925_test_mt7927_sublink_roc) &&
	    is_mt7927(&dev->mt76) && (created & add) == add &&
	    hweight16(add) == 1 && hweight16(old_links & ~rem) == 1) {
		unsigned int sub_link_id = __ffs(add);
		struct mt792x_bss_conf *sub_mconf =
			mt7927_mlo_resident_bss(dev, mvif, sub_link_id);
		struct ieee80211_bss_conf *sub_link_conf =
			mt792x_vif_to_bss_conf(vif, sub_link_id);

		if (sub_mconf && sub_link_conf && sub_link_conf->chanreq.oper.chan) {
			err = mt7925_set_sublink_roc(phy, sub_mconf,
						     sub_link_conf->chanreq.oper.chan,
						     sub_mconf->mt76.band_idx);
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "SUBLINK_ROC_TEST: result=%d link_id=%u bss_idx=%u\n",
				      err, sub_link_id, sub_mconf->mt76.idx);
			if (err)
				goto err_remove_created;

			mt792x_mutex_release(dev);
			return 0;
		}
	}

	if (add) {
		if ((READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)) &&
		    mt7927_mlo_join_retry_allowed(dev, vif) && (created & add))
			printk(KERN_INFO
			      "MLO_ROC_RETRY: attempt=1/2 add=0x%lx created=0x%lx sel_links=0x%x\n",
			      add, created, new_links);

		if (mvif->mt7927_mlo.join_deferred) {
			printk(KERN_INFO
			       "MLO_JOIN_DEFER_RESUME: wanted=0x%x new_links=0x%x gen=%u cur_gen=%u partner_bss_ready=%d\n",
			       mvif->mt7927_mlo.join_deferred_links, new_links,
			       mvif->mt7927_mlo.join_deferred_gen,
			       mvif->mt7927_mlo.transition_generation,
			       !!((created | mvif->valid_links) & add));
		}

		err = mt7925_set_mlo_roc(phy, &mvif->bss_conf, new_links);

		if (mvif->mt7927_mlo.join_deferred) {
			printk(KERN_INFO "MLO_JOIN_DEFER_RESUME: result=%d token=%u\n",
			       err, phy->roc_token_id);
			mvif->mt7927_mlo.join_deferred = false;
			mvif->mt7927_mlo.join_deferred_links = 0;
		}

		if (err == -ETIMEDOUT && (created & add) &&
		    mt7927_mlo_join_retry_allowed(dev, vif)) {
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_ROC_RETRY: attempt=2/2 add=0x%lx created=0x%lx sel_links=0x%x\n",
				      add, created, new_links);
			err = mt7925_set_mlo_roc(phy, &mvif->bss_conf, new_links);
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_ROC_RETRY: attempt=2/2 result=%d\n", err);
		}

		if (err == -ETIMEDOUT && !(created & add)) {
			dev_warn(dev->mt76.dev,
				 "MT7927 MLO: resident link re-entry continuing after ROC timeout links=0x%x add=0x%lx\n",
				 new_links, add);
			err = 0;
		} else if (err == -ETIMEDOUT && (created & add) &&
			   READ_ONCE(mt7925_test_mt7927_mlo_noroc_admit)) {
			unsigned int admit_link_id;
			unsigned long admit_bits = created & add;

			for_each_set_bit(admit_link_id, &admit_bits, IEEE80211_MLD_MAX_NUM_LINKS)
				dev_warn(dev->mt76.dev,
					 "MUP: noroc_admit bypassing ROC failure for link_id=%u\n",
					 admit_link_id);
			err = 0;
		} else if (err) {
			goto err_remove_created;
		}
	}

	mt792x_mutex_release(dev);
	return 0;

err_remove_created:
	for_each_set_bit(link_id, &created, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *mconf;
		struct mt792x_link_sta *mlink;

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_RESIDENT_TRACE: fn=err_unwind link=%u err=%d\n",
			      link_id, err);

		mconf = mt7927_mlo_resident_bss(dev, mvif, link_id);
		mlink = mt7927_mlo_resident_sta(dev, &mvif->sta, link_id);
		if (mconf && mlink)
			mt792x_mac_link_bss_remove(dev, mconf, mlink);
		rcu_assign_pointer(mvif->link_conf[link_id], NULL);
		rcu_assign_pointer(mvif->sta.link[link_id], NULL);
		if (mconf && mconf != &mvif->bss_conf)
			devm_kfree(dev->mt76.dev, mconf);
		if (mlink && mlink != &mvif->sta.deflink)
			devm_kfree(dev->mt76.dev, mlink);
		mvif->valid_links &= ~BIT(link_id);
		mvif->mt7927_mlo.resident_links &= ~BIT(link_id);
		mvif->mt7927_mlo.link[link_id].band_moved = false;
	}
	mt792x_mutex_release(dev);
	return err;
}

/* Prepare-only half of postkey grant activation: create or reuse the
 * substitute's peer link object, issue the required firmware STA_REC
 * creation, and reconcile its keys with the error actually propagated --
 * unlike the bundled mt7925_mac_sta_add_links(), which unconditionally
 * publishes msta->valid_links and calls mt7925_mac_link_sta_assoc() while
 * silently discarding mt7925_mlo_reconcile_link()'s return value. Never
 * sets msta->valid_links, wcid.link_valid, associates, or touches
 * traffic_active_links -- the caller commits those only after this
 * returns 0. On failure for a peer THIS call created, undoes it with the
 * real firmware removal primitive (mt7925_mac_link_sta_remove(), which
 * sends MT76_STA_INFO_STATE_NONE) rather than kfree_rcu() alone, which
 * would leak the firmware STA_REC. A pre-existing resident peer is left
 * untouched on failure -- this function does not own its lifetime.
 *
 * *out_created reports whether this call itself invoked
 * mt7925_mac_sta_add_links() -- which, for a non-deflink id, unconditionally
 * sends its own association command internally. The caller must not
 * associate a second time when this is true, or a real grant would send
 * two association commands for the same link.
 */
static int
mt7927_mlo_postkey_prepare(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			   struct ieee80211_sta *sta, struct mt792x_sta *msta,
			   unsigned int pend_sub,
			   struct mt792x_link_sta **out_mlink, bool *out_created)
{
	struct mt792x_link_sta *mlink = mt7927_mlo_resident_sta(dev, msta, pend_sub);
	struct ieee80211_link_sta *link_sta = NULL;
	bool created = false;
	int err;

	*out_created = false;

	if (!mlink) {
		err = mt7925_mac_sta_add_links(dev, vif, sta, BIT(pend_sub));
		if (err)
			return err;
		created = true;
		*out_created = true;
		mlink = mt7927_mlo_resident_sta(dev, msta, pend_sub);
		if (!mlink)
			return -EINVAL;
		/* mt7925_mac_sta_add_links() just did the very thing this
		 * split exists to avoid: it published msta->valid_links, sent
		 * the STA association command, and silently discarded its
		 * own internal reconcile's error. Force this link back to
		 * "created but not yet admitted" and redo reconcile below
		 * where the result IS checked.
		 */
		msta->valid_links &= ~BIT(pend_sub);
		mlink->wcid.link_valid = false;
	}

	link_sta = mt792x_sta_to_link_sta(vif, sta, pend_sub);
	if (!link_sta) {
		err = -EINVAL;
		goto fail;
	}

	err = mt7925_mlo_reconcile_link(dev, vif, sta, mlink, pend_sub);
	if (err)
		goto fail;

	*out_mlink = mlink;
	return 0;

fail:
	if (created) {
		if (link_sta)
			mt7925_mac_link_sta_remove(&dev->mt76, vif, link_sta,
						   mlink);
		rcu_assign_pointer(msta->link[pend_sub], NULL);
		msta->valid_links &= ~BIT(pend_sub);
		kfree_rcu(mlink, rcu_head);
	}
	return err;
}

/* Post-key-switch standalone SUB_LINK ROC experiment. The real gate is
 * explicit pending-state validity (peer-owned, recorded by
 * mt7927_mlo_change_vif_links()'s completion phase) plus verified
 * group-key readiness via the shared mt7925_mlo_group_key_rebind() helper.
 * Only ever reached now via the STA-owned two-stage SET_KEY trigger
 * (stage2, below) -- stage1/stage2 only run after the wiphy-work barrier,
 * i.e. after mac80211's entire ieee80211_key_switch_links() loop (covering
 * GTK/IGTK/BIGTK) has returned, so keys_seen==0 here is a real "no group
 * key for this link" signal, not a race: it is terminal.
 *
 * Resolves the substitute/survivor resident objects fresh on every call
 * rather than caching them across any async boundary.
 *
 * "_locked": assumes dev->mt76.mutex is already held by the caller
 * (stage2, which acquires it itself before calling in).
 */
static void
mt7927_mlo_postkey_attempt_locked(struct mt792x_dev *dev, struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif, struct ieee80211_sta *sta,
				  u16 new_links)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	struct mt7925_group_key_rebind_result key_result = {};
	struct mt792x_bss_conf *sub_mconf;
	struct mt792x_link_sta *surv_mlink;
	unsigned int pend_sub, pend_surv;
	int key_err;

	if (!msta->postkey_pending)
		return;

	pend_sub = msta->postkey_sublink_id;
	pend_surv = msta->postkey_survivor_id;
	sub_mconf = mt7927_mlo_resident_bss(dev, mvif, pend_sub);
	surv_mlink = mt7927_mlo_resident_sta(dev, msta, pend_surv);

	if (msta->postkey_roc_attempted ||
	    msta->postkey_generation != mvif->mt7927_mlo.transition_generation ||
	    !(new_links & BIT(pend_sub)) ||
	    !sub_mconf ||
	    !surv_mlink || !surv_mlink->wcid.link_valid) {
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "POSTKEY_KEEP_DORMANT: reason=structural_invalid substitute=%u survivor=%u gen=%u cur_gen=%u\n",
			      pend_sub, pend_surv,
			      msta->postkey_generation,
			      mvif->mt7927_mlo.transition_generation);
		mt7927_mlo_pending_clear(msta);
		return;
	}

	key_err = mt7925_mlo_group_key_rebind(dev, vif, pend_sub, &key_result);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "POSTKEY_KEYS_READY: substitute=%u keys_seen=%u gtk=%u igtk=%u bigtk=%u already_current=%u installs_attempted=%u installs_succeeded=%u err=%d\n",
		      pend_sub, key_result.keys_seen, key_result.gtk_seen,
		      key_result.igtk_seen, key_result.bigtk_seen,
		      key_result.already_current, key_result.installs_attempted,
		      key_result.installs_succeeded, key_err);

	if (key_result.keys_seen == 0) {
		/* Stage2 only runs after the wiphy-work barrier -- mac80211's
		 * entire key_switch_links() loop for this link has already
		 * returned -- so no group key here is a real, terminal
		 * signal, not a race to retry.
		 */
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "POSTKEY_KEYS_UNAVAILABLE: substitute=%u\n",
			      pend_sub);
		mt7927_mlo_pending_clear(msta);
		return;
	}

	if (key_err) {
		mt7927_mlo_pending_clear(msta);
		return;
	}

	/* From here on, this pending record is consumed exactly once,
	 * regardless of outcome.
	 */
	msta->postkey_roc_attempted = true;

	{
		struct mt792x_phy *phy = mt792x_hw_phy(hw);
		struct ieee80211_bss_conf *sub_link_conf =
			mt792x_vif_to_bss_conf(vif, pend_sub);
		int roc_err = -EINVAL;

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "POSTKEY_ROC_REQ: substitute=%u bss_idx=%u band_idx=%u\n",
			      pend_sub, sub_mconf->mt76.idx,
			      sub_mconf->mt76.band_idx);

		if (sub_link_conf && sub_link_conf->chanreq.oper.chan)
			roc_err = mt7925_set_sublink_roc(phy, sub_mconf,
							 sub_link_conf->chanreq.oper.chan,
							 sub_mconf->mt76.band_idx);

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "POSTKEY_ROC_RESULT: substitute=%u result=%d granted=%d\n",
			      pend_sub, roc_err, roc_err == 0);

		if (roc_err) {
			mt7927_mlo_pending_clear(msta);
			return;
		}
	}

	/* Real grant -- prepare then commit: the substitute must never
	 * become traffic-eligible unless prepare (peer STA_REC creation and
	 * key reconcile, with errors actually checked) fully succeeds.
	 */
	{
		struct mt792x_link_sta *mlink = NULL;
		bool created = false;
		int act_err;

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "POSTKEY_PREPARE: substitute=%u\n",
			      pend_sub);

		act_err = mt7927_mlo_postkey_prepare(dev, vif, sta, msta,
						     pend_sub, &mlink, &created);

		if (!act_err) {
			struct ieee80211_link_sta *link_sta =
				mt792x_sta_to_link_sta(vif, sta, pend_sub);

			if (!link_sta) {
				act_err = -EINVAL;
			} else {
				mvif->mt7927_mlo.link[pend_sub].adding = true;
				mlink->wcid.link_valid = true;
				/* mt7925_mac_sta_add_links() (inside prepare,
				 * only when it created the peer) already sent
				 * its own association command internally --
				 * do not send a second one for the same link.
				 */
				if (!created) {
					act_err = mt7925_mac_link_sta_assoc(dev, vif,
								      link_sta);
					if (act_err)
						goto activation_failed;
				}
				mt7927_mlo_note_sta(mvif, mlink, true);
				mvif->mt7927_mlo.link[pend_sub].roc_admitted = true;
				mvif->mt7927_mlo.link[pend_sub].admitted_generation =
					mvif->mt7927_mlo.transition_generation;
				msta->valid_links |= BIT(pend_sub);
				mvif->mt7927_mlo.traffic_active_links |= BIT(pend_sub);
				mvif->mt7927_mlo.link[pend_sub].adding = false;

				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "POSTKEY_ACTIVATE: substitute=%u gen=%u wcid_valid=%d\n",
					      pend_sub,
					      mvif->mt7927_mlo.transition_generation,
					      mlink->wcid.link_valid);
			}
		}

activation_failed:
		if (act_err && mlink) {
			mlink->wcid.link_valid = false;
			mvif->mt7927_mlo.link[pend_sub].adding = false;
		}
		if (act_err && (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
			printk(KERN_INFO
			      "POSTKEY_KEEP_DORMANT: reason=activation_failed substitute=%u err=%d\n",
			      pend_sub, act_err);
	}

	mt7927_mlo_pending_clear(msta);
}

/* STA-owned two-stage post-key ROC trigger, see the mt7925.h prototype
 * comment. Stage 1 (wiphy_work): only queues stage2 and returns -- no MCU
 * command, ROC, wait, or mt76 mutex acquisition, so the wiphy mutex it
 * runs under is never held for anything blocking.
 */
void mt7927_mlo_postkey_stage1_work(struct wiphy *wiphy,
				    struct wiphy_work *work)
{
	struct mt792x_sta *msta = container_of(work, struct mt792x_sta,
					       postkey_stage1);
	struct mt792x_vif *mvif = msta->vif;
	struct mt792x_dev *dev;

	if (!mvif)
		return;

	dev = mvif->phy->dev;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "POSTKEY_STAGE1: pending=%d sublink=%u survivor=%u gen=%u\n",
		      msta->postkey_pending, msta->postkey_sublink_id,
		      msta->postkey_survivor_id, msta->postkey_generation);

	if (!msta->postkey_pending)
		return;

	ieee80211_queue_work(mt76_hw(dev), &msta->postkey_stage2);
}

/* Stage 2 (ordinary work_struct): derives every object fresh from
 * proven-resident, driver-owned state -- never an RCU-only pointer
 * retained past its critical section -- acquires dev->mt76.mutex exactly
 * once, and runs the real (blocking) postkey attempt.
 */
void mt7927_mlo_postkey_stage2_work(struct work_struct *work)
{
	struct mt792x_sta *msta = container_of(work, struct mt792x_sta,
					       postkey_stage2);
	struct mt792x_vif *mvif;
	struct ieee80211_vif *vif;
	struct ieee80211_sta *sta;
	struct mt792x_dev *dev;
	struct ieee80211_hw *hw;

	mvif = msta->vif;
	if (!mvif)
		return;

	vif = container_of((void *)mvif, struct ieee80211_vif, drv_priv);
	dev = mvif->phy->dev;
	hw = mt76_hw(dev);
	sta = wcid_to_sta(&msta->deflink.wcid);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "POSTKEY_STAGE2: pending=%d has_sta=%d\n",
		      msta->postkey_pending, !!sta);

	if (!sta || !msta->postkey_pending)
		return;

	mt792x_mutex_acquire(dev);
	mt7927_mlo_postkey_attempt_locked(dev, hw, vif, sta, vif->active_links);
	mt792x_mutex_release(dev);
}

/* Announce a freshly admitted link awake to the AP. The AP keeps every affiliated link that has never carried a frame
 * in power save (hardware: AP PSMODE stayed 1 for 40 s and its link node counted 0/0 while the DUT sent nothing through
 * the new WCID), so it never schedules downlink there and the RX-progress proof below can never complete. The probe is
 * one unprotected, PM-clear Null/QoS-Null frame built by mac80211 for this link's own addresses and sent through the
 * generic TX entry with an explicit link, so it leaves on this link's WCID. It carries no user data, touches no
 * counter, key or PN, and publishes no readiness: readiness still requires real authenticated RX.
 */
static int mt7927_mlo_link_wake_probe(struct mt792x_dev *dev,
				      struct ieee80211_vif *vif,
				      struct ieee80211_sta *sta,
				      struct mt792x_link_sta *peer,
				      unsigned int link_id)
{
	struct ieee80211_hw *hw = mt76_hw(dev);
	struct ieee80211_tx_control control = { .sta = sta };
	struct ieee80211_tx_info *info;
	struct sk_buff *skb;

	skb = ieee80211_nullfunc_get(hw, vif, link_id, true);
	if (!skb)
		return -ENOMEM;

	info = IEEE80211_SKB_CB(skb);
	info->control.vif = vif;
	info->control.flags |= u32_encode_bits(link_id, IEEE80211_TX_CTRL_MLO_LINK);
	info->flags |= IEEE80211_TX_CTL_REQ_TX_STATUS;
	mt792x_tx(hw, &control, skb);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_LINK_WAKE_PROBE: link=%u wcid=%u\n",
		       link_id, peer->wcid.idx);
	return 0;
}

/* Fresh-admission retry stimulus for mt7927_mlo_wait_link_rx_ready(): unlike
 * migration, a brand-new substitute link's WCID was never CREATEd more than
 * once and never rebinds to a different phy, so re-issuing
 * mt7925_mcu_sta_rebind_phy() (mt7927_mlo_recreate_authorize_peer()'s
 * "create" stage) does not apply here. What genuinely can be reissued as a
 * real firmware control-plane stimulus -- not ordinary user data, not a
 * fabricated counter -- is the same RESTORE (mt7925_mlo_reconcile_link())
 * + AUTHORIZE (mt7925_mac_link_sta_assoc()) MCU exchange this link already
 * went through once in the add-loop below.
 */
struct mt7927_mlo_admit_retry_ctx {
	struct mt792x_dev *dev;
	struct ieee80211_vif *vif;
	struct ieee80211_sta *sta;
	struct mt792x_sta *msta;
	struct ieee80211_link_sta *link_sta;
	struct mt792x_link_sta *peer;
};

static int
mt7927_mlo_reauthorize_added_link(void *ctx_v, unsigned int link_id, bool retry)
{
	struct mt7927_mlo_admit_retry_ctx *ctx = ctx_v;
	struct mt792x_vif *mvif = (struct mt792x_vif *)ctx->vif->drv_priv;
	int err;

	mt7927_mlo_link_wake_probe(ctx->dev, ctx->vif, ctx->sta, ctx->peer,
				   link_id);
	err = mt7925_mlo_reconcile_link(ctx->dev, ctx->vif, ctx->sta, ctx->peer,
					link_id);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_FW_PEER_LIFECYCLE: generation=%u transition_from=0x%x transition_to=0x%x stage=admit_retry_restore link_id=%u wcid=%u phy_idx=%u state=%u newly=0 hw_key_idx=%u link_valid=%u ready_ts=%lu ret=%d\n",
		      mvif->mt7927_mlo.transition_generation,
		      mvif->mt7927_mlo.migration_from_links,
		      mvif->mt7927_mlo.migration_to_links, link_id,
		      ctx->peer->wcid.idx, ctx->peer->wcid.phy_idx,
		      MT76_STA_INFO_STATE_NONE, ctx->peer->wcid.hw_key_idx,
		      ctx->peer->wcid.link_valid,
		      ctx->msta->mlo_link_ready_jiffies[link_id], err);
	if (err)
		return err;

	err = mt7925_mac_link_sta_assoc(ctx->dev, ctx->vif, ctx->link_sta);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_FW_PEER_LIFECYCLE: generation=%u transition_from=0x%x transition_to=0x%x stage=admit_retry_authorize link_id=%u wcid=%u phy_idx=%u state=%u newly=0 hw_key_idx=%u link_valid=%u ready_ts=%lu ret=%d\n",
		      mvif->mt7927_mlo.transition_generation,
		      mvif->mt7927_mlo.migration_from_links,
		      mvif->mt7927_mlo.migration_to_links, link_id,
		      ctx->peer->wcid.idx, ctx->peer->wcid.phy_idx,
		      MT76_STA_INFO_STATE_ASSOC, ctx->peer->wcid.hw_key_idx,
		      ctx->peer->wcid.link_valid,
		      ctx->msta->mlo_link_ready_jiffies[link_id], err);

	return err;
}

static void mt7927_mlo_rb_trace(struct ieee80211_vif *vif,
				struct mt792x_vif *mvif, const char *phase,
				unsigned int link, struct mt792x_link_sta *peer,
				struct mt792x_link_sta *bmc,
				struct mt792x_bss_conf *mconf, int result)
{
	if (!(READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
		return;

	printk(KERN_INFO
	       "MLO_SEC_REBIND: id=%u phase=%s link=%u wcid=%d bss_idx=%d phy_idx=%d hw_key_idx=%d/%d bmc_wcid=%d bmc_key=%d/%d valid_rx=%u active_links=0x%x valid_links=0x%x target=0x%x result=%d mono_us=%llu rel_T0_us=%lld\n",
	       mvif->mt7927_mlo.transition_generation, phase, link,
	       peer ? (int)peer->wcid.idx : -1, mconf ? (int)mconf->mt76.idx : -1,
	       peer ? (int)peer->wcid.phy_idx : -1,
	       peer ? (int)peer->wcid.hw_key_idx : -1,
	       peer ? (int)peer->wcid.hw_key_idx2 : -1,
	       bmc ? (int)bmc->wcid.idx : -1,
	       bmc ? (int)bmc->wcid.hw_key_idx : -1,
	       bmc ? (int)bmc->wcid.hw_key_idx2 : -1,
	       peer ? READ_ONCE(peer->mlo_valid_rx_packets) : 0,
	       vif->active_links, vif->valid_links,
	       mvif->mt7927_mlo.rebind_target_links, result,
	       (unsigned long long)(ktime_get_ns() / 1000),
	       (long long)div_s64((s64)(ktime_get_ns() -
					mvif->mt7927_mlo.pt_ns[0]), 1000));
}

#define MT7927_MLO_REBIND_RX_WINDOW_MS	4000

static void mt7927_mlo_parent_trace(struct ieee80211_vif *vif,
				    struct mt792x_vif *mvif, const char *tag,
				    unsigned int link,
				    struct mt792x_bss_conf *mconf,
				    struct ieee80211_bss_conf *link_conf,
				    struct mt792x_link_sta *bmc,
				    struct mt792x_link_sta *peer, int result)
{
	const struct cfg80211_chan_def *cd = link_conf ?
		&link_conf->chanreq.oper : NULL;

	if (!(READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
		return;

	printk(KERN_INFO
	       "%s: id=%u link_id=%u bss_idx=%d omac=%d band_idx=%d link_idx=%d chan_band=%d channel=%d center=%u width=%d ctx=%d bmc_wcid=%d peer_wcid=%d active_links=0x%x valid_links=0x%x target=0x%x ret=%d mono_us=%llu rel_T0_us=%lld\n",
	       tag, mvif->mt7927_mlo.transition_generation, link,
	       mconf ? (int)mconf->mt76.idx : -1,
	       mconf ? (int)mconf->mt76.omac_idx : -1,
	       mconf ? (int)mconf->mt76.band_idx : -1,
	       mconf ? (int)mconf->mt76.link_idx : -1,
	       cd && cd->chan ? (int)cd->chan->band : -1,
	       cd && cd->chan ? (int)cd->chan->hw_value : -1,
	       cd ? cd->center_freq1 : 0, cd ? (int)cd->width : -1,
	       mconf && mconf->mt76.ctx ? 1 : 0,
	       bmc ? (int)bmc->wcid.idx : -1, peer ? (int)peer->wcid.idx : -1,
	       vif->active_links, vif->valid_links,
	       mvif->mt7927_mlo.rebind_target_links, result,
	       (unsigned long long)(ktime_get_ns() / 1000),
	       (long long)div_s64((s64)(ktime_get_ns() -
				  mvif->mt7927_mlo.pt_ns[0]), 1000));
}

/* Post-regrant parent rebuild (test gate mlo_regrant_parent_rebuild).
 *
 * After the full JOIN re-grants the radio group, re-send the parent context
 * of BOTH active links (survivor included) against the new grants: DEV_INFO
 * for every link, then BSS_INFO (BASIC, SEC, BMC, QOS, MLD, IFS, HE, COLOR,
 * RLM with the current channel/band/width, MBSSID) for every link. Same
 * enable re-send the normal link-add (mt7925_mac_link_bss_add ->
 * mt76_connac_mcu_uni_add_dev) and the in-place band move
 * (mt7927_mlo_reassign_band) use. No disable is issued: every MLD link shares
 * OMAC 0, so a DEV_INFO disable would tear the live MLD device down. Neither
 * helper short-circuits on host state, so each call is a real MCU message
 * (MLO_FW_DEV_BSS_RAW / MLO_FW_BSS_RAW trace it). The first failure stops the
 * pass; the caller then runs no child security phase.
 */
static int mt7927_mlo_regrant_parent_rebuild(struct mt792x_dev *dev,
					     struct ieee80211_vif *vif,
					     struct ieee80211_sta *sta,
					     struct mt792x_sta *msta,
					     unsigned long links,
					     enum mt7925_mlo_pt_parent_step *out)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	enum mt7925_mlo_pt_parent_step step = MT7925_MLO_PT_PARENT_NONE;
	unsigned int lid;
	int err = 0;

	for (step = mt7925_mlo_pt_parent_next(step, 0);
	     step != MT7925_MLO_PT_PARENT_DONE &&
	     step != MT7925_MLO_PT_PARENT_FAILED;
	     step = mt7925_mlo_pt_parent_next(step, err)) {
		for_each_set_bit(lid, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_bss_conf *mconf =
				mt7927_mlo_resident_bss(dev, mvif, lid);
			struct ieee80211_bss_conf *link_conf =
				mt792x_vif_to_bss_conf(vif, lid);
			struct mt792x_link_sta *bmc =
				mt7927_mlo_resident_sta(dev, &mvif->sta, lid);
			struct mt792x_link_sta *peer =
				mt7927_mlo_resident_sta(dev, msta, lid);
			struct ieee80211_link_sta *link_sta =
				mt792x_sta_to_link_sta(vif, sta, lid);

			if (!mconf || !link_conf || !bmc || !peer || !link_sta) {
				err = -EINVAL;
				mt7927_mlo_parent_trace(vif, mvif,
							"MLO_REGRANT_PARENT_MISSING",
							lid, mconf, link_conf, bmc,
							peer, err);
				break;
			}

			if (step == MT7925_MLO_PT_PARENT_DEV) {
				err = mt76_connac_mcu_uni_add_dev(&dev->mphy,
								  link_conf,
								  &mconf->mt76,
								  &bmc->wcid, true);
				mt7927_mlo_parent_trace(vif, mvif,
							"MLO_REGRANT_DEV_REBUILD", lid,
							mconf, link_conf, bmc, peer,
							err);
			} else {
				err = mt7925_mcu_add_bss_info(&dev->phy,
							      mconf->mt76.ctx,
							      link_conf, link_sta,
							      true);
				mt7927_mlo_parent_trace(vif, mvif,
							"MLO_REGRANT_BSS_REBUILD", lid,
							mconf, link_conf, bmc, peer,
							err);
			}
			if (err)
				break;
		}
	}

	*out = step;
	return step == MT7925_MLO_PT_PARENT_DONE ? 0 : (err ? err : -EIO);
}

/* Post-regrant security rebind (test gate mlo_security_rebind).
 *
 * The logical MLD is unchanged; only the firmware-local binding of each
 * active link to the old radio-group incarnation is stale. Ordered pass over
 * BOTH active links, survivor included, after both post-grant WCID/BSS
 * objects exist:
 *   INVALIDATE  stale hw_key_idx (peer and BMC) so nothing is suppressed
 *   STA_REC     remove-first rebuild to STATE_NONE for each link
 *   PTK         existing PTK re-pushed into each current WCID
 *   GTK         group keys re-pushed for each current BSS/BMC
 *   AUTHORIZE   STATE_ASSOC + PORT_SECURE; STA_REC_MLD is built here from the
 *               post-grant target set (rebind_target_links)
 *   WAIT_RX     security-valid RX required independently on BOTH links
 * The first failing command stops the sequence. The transition reaches
 * CONVERGED only when every active link has proven security-valid RX.
 * Always returns 0 so mac80211 never unwinds the association.
 */
static int mt7927_mlo_security_rebind_pair(struct mt792x_dev *dev,
					   struct ieee80211_vif *vif,
					   struct ieee80211_sta *sta,
					   struct mt792x_sta *msta,
					   u16 new_links)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long links = mt7925_mlo_pt_rebind_links(
		mvif->mt7927_mlo.radio_group_reincarnated, new_links);
	enum mt7925_mlo_pt_rebind_phase ph = MT7925_MLO_PT_REBIND_NONE;
	enum mt7925_mlo_pt_parent_step pstep = MT7925_MLO_PT_PARENT_NONE;
	u32 base[IEEE80211_MLD_MAX_NUM_LINKS] = {};
	unsigned int secure = 0, lid;
	int err = 0;

	if (!links)
		return 0;

	mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_RECOVER_SURVIVOR,
				"security_rebind_begin");
	mt7927_mlo_pt_stamp(mvif, 4);

	/* Parent before child: with mlo_regrant_parent_rebuild the DEV/BSS of
	 * BOTH links is rebuilt against the new grants first; no child security
	 * phase runs unless that pass completed.
	 */
	if (mt7925_mlo_pt_parent_links(mt7925_mlo_regrant_parent_rebuild, links))
		err = mt7927_mlo_regrant_parent_rebuild(dev, vif, sta, msta,
							mt7925_mlo_pt_parent_links(
								1, links),
							&pstep);
	if (!mt7925_mlo_pt_child_allowed(mt7925_mlo_regrant_parent_rebuild,
					 pstep))
		ph = MT7925_MLO_PT_REBIND_FAILED;

	for (ph = ph == MT7925_MLO_PT_REBIND_FAILED ? ph :
		  mt7925_mlo_pt_rebind_next(ph, 0);
	     ph != MT7925_MLO_PT_REBIND_DONE && ph != MT7925_MLO_PT_REBIND_FAILED;
	     ph = mt7925_mlo_pt_rebind_next(ph, err)) {
		if (ph == MT7925_MLO_PT_REBIND_WAIT_RX) {
			u64 end = ktime_get_ns() +
				  (u64)MT7927_MLO_REBIND_RX_WINDOW_MS * NSEC_PER_MSEC;

			mt7927_mlo_pt_set_state(vif, mvif,
						MT7925_MLO_PT_WAIT_REPLACEMENT_RX,
						"security_rebind_wait_rx");
			do {
				for_each_set_bit(lid, &links,
						 IEEE80211_MLD_MAX_NUM_LINKS) {
					struct mt792x_link_sta *peer =
						mt7927_mlo_resident_sta(dev, msta, lid);

					if (peer && READ_ONCE(peer->mlo_valid_rx_packets) !=
					    base[lid])
						secure |= BIT(lid);
				}
				if (mt7925_mlo_pt_converge_allowed(links, secure))
					break;
				usleep_range(3000, 5000);
			} while (ktime_get_ns() < end);

			for_each_set_bit(lid, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
				struct mt792x_link_sta *peer =
					mt7927_mlo_resident_sta(dev, msta, lid);
				struct mt792x_link_sta *bmc =
					mt7927_mlo_resident_sta(dev, &mvif->sta, lid);
				struct mt792x_bss_conf *mconf =
					mt7927_mlo_resident_bss(dev, mvif, lid);
				bool ok = secure & BIT(lid);

				mt7927_mlo_rb_trace(vif, mvif,
						    ok ? "rx_ready" : "rx_not_ready",
						    lid, peer, bmc, mconf, ok ? 0 : -ENODATA);
				if (ok && peer) {
					msta->mlo_link_ready_jiffies[lid] = jiffies;
					WRITE_ONCE(peer->mlo_await_valid_rx, false);
				}
			}
			err = 0;
			continue;
		}

		for_each_set_bit(lid, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_link_sta *peer =
				mt7927_mlo_resident_sta(dev, msta, lid);
			struct mt792x_link_sta *bmc =
				mt7927_mlo_resident_sta(dev, &mvif->sta, lid);
			struct mt792x_bss_conf *mconf =
				mt7927_mlo_resident_bss(dev, mvif, lid);
			struct ieee80211_link_sta *link_sta =
				mt792x_sta_to_link_sta(vif, sta, lid);
			struct mt7925_group_key_rebind_result kr = {};
			const char *name = "";

			if (!peer || !bmc || !mconf || !link_sta) {
				err = -EINVAL;
				mt7927_mlo_rb_trace(vif, mvif, "missing_objects",
						    lid, peer, bmc, mconf, err);
				break;
			}

			switch (ph) {
			case MT7925_MLO_PT_REBIND_INVALIDATE:
				name = "invalidate";
				mt7927_mlo_rb_trace(vif, mvif, "invalidate_before",
						    lid, peer, bmc, mconf, 0);
				if (mt7925_mlo_pt_key_reprogram_needed(
					    peer->wcid.hw_key_idx, 1)) {
					peer->wcid.hw_key_idx = 255;
					peer->wcid.hw_key_idx2 = 255;
				}
				bmc->wcid.hw_key_idx = 255;
				bmc->wcid.hw_key_idx2 = 255;
				msta->mlo_link_ready_jiffies[lid] = 0;
				peer->tx_liveness_rx_seen = peer->mlo_valid_rx_packets;
				peer->tx_liveness_tx_seen = peer->wcid.stats.tx_packets;
				peer->tx_liveness_rx_ever_seen = false;
				peer->tx_liveness_rx_progress_jiffies = 0;
				base[lid] = READ_ONCE(peer->mlo_valid_rx_packets);
				err = 0;
				break;
			case MT7925_MLO_PT_REBIND_STA_REC:
				name = "sta_rec_remove_create_none";
				err = mt7925_mcu_sta_update(dev, link_sta, vif, peer,
							    false,
							    MT76_STA_INFO_STATE_NONE, NULL);
				if (!err)
					err = mt7925_mcu_sta_update(dev, link_sta, vif,
								    peer, true,
								    MT76_STA_INFO_STATE_NONE, NULL);
				peer->wcid.link_valid = true;
				break;
			case MT7925_MLO_PT_REBIND_PTK:
				name = "ptk_rebind";
				err = mt7925_mlo_ptk_rebind(dev, vif, sta, peer);
				break;
			case MT7925_MLO_PT_REBIND_GTK:
				name = "gtk_rebind";
				err = mt7925_mlo_group_key_rebind(dev, vif, lid, &kr);
				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					       "MLO_SEC_REBIND_GTK: id=%u link=%u keys_seen=%u gtk=%u igtk=%u bigtk=%u already_current=%u installs_attempted=%u installs_succeeded=%u result=%d\n",
					       mvif->mt7927_mlo.transition_generation,
					       lid, kr.keys_seen, kr.gtk_seen,
					       kr.igtk_seen, kr.bigtk_seen,
					       kr.already_current,
					       kr.installs_attempted,
					       kr.installs_succeeded, err);
				break;
			case MT7925_MLO_PT_REBIND_AUTHORIZE:
				name = "authorize_assoc_mld";
				err = mt7925_mcu_sta_authorize(dev, link_sta, vif, peer);
				break;
			default:
				err = -EINVAL;
				break;
			}

			mt7927_mlo_rb_trace(vif, mvif, name, lid, peer, bmc, mconf,
					    err);
			if (err)
				break;
		}
	}

	if (mt7925_mlo_pt_converge_allowed(links, secure)) {
		mt7927_mlo_pt_stamp(mvif, 5);
		mt7927_mlo_pt_stamp(mvif, 8);
		mt7927_mlo_pt_trace(vif, mvif, "MLO_PAIR_TRANSITION_RX_READY",
				    "both_links_secure_rx", secure);
		mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_MIGRATE_TRAFFIC,
					"both_links_secure_rx");
		mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_RELEASE_OBSOLETE,
					"removed_slots_already_released");
		mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_CONVERGED,
					"converged");
		mt7927_mlo_pt_stamp(mvif, 9);
		mt7927_mlo_pt_trace(vif, mvif, "MLO_PAIR_TRANSITION_COMMIT",
				    "commit", 0);
		mt7927_mlo_pt_timeline(vif, mvif, "commit");
		mt7927_mlo_pt_set_state(vif, mvif, MT7925_MLO_PT_IDLE, "done");
	} else {
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			       "MLO_SEC_REBIND_RESULT: id=%u insufficient links=0x%lx secure_rx=0x%x phase_err=%d parent_rebuild=%d parent_step=%d\n",
			       mvif->mt7927_mlo.transition_generation, links,
			       secure, err, mt7925_mlo_regrant_parent_rebuild,
			       pstep);
		mt7927_mlo_pt_failsafe(vif, mvif, &dev->phy,
				       "security_rebind_insufficient",
				       err ? err : -ENODATA);
	}

	mvif->mt7927_mlo.rebind_target_links = 0;
	mvif->mt7927_mlo.radio_group_reincarnated = false;
	return 0;
}

/* EXPERIMENT (default N): WCID handover. The association's first WCID (the deflink) is the firmware MLD's key and TX PN
 * holder; frames addressed to any other WCID carry that WCID's own, lower PN and the AP silently drops them. When a
 * pair transition retires the deflink and the incoming link uses the same firmware band slot, the incoming link takes
 * over the deflink's WCID (same WTBL entry, key and PN counter, no key reinstall) instead of getting a new one.
 * Preconditions are checked by mt7925_mlo_handover_donor(); on any doubt the stock path runs.
 */
static bool mt7925_mlo_wcid_handover;
module_param_named(mlo_wcid_handover, mt7925_mlo_wcid_handover, bool, 0644);
MODULE_PARM_DESC(mlo_wcid_handover,
		 "EXPERIMENTAL: incoming MT7927 MLD link inherits the retired deflink's WCID on the same band slot (default N)");

static bool mt7927_mlo_try_wcid_handover(struct mt792x_dev *dev, struct ieee80211_vif *vif,
					 struct ieee80211_sta *sta, struct mt792x_sta *msta,
					 struct mt792x_vif *mvif, unsigned int link_id)
{
	unsigned int band[IEEE80211_MLD_MAX_NUM_LINKS] = {};
	unsigned long resident = 0, live = 0;
	struct ieee80211_link_sta *link_sta;
	struct mt792x_bss_conf *in_conf;
	struct mt792x_link_sta *donor;
	unsigned int id;
	int donor_id;

	if (!READ_ONCE(mt7925_mlo_wcid_handover) || !is_mt7927(&dev->mt76) ||
	    msta->deflink_id >= IEEE80211_MLD_MAX_NUM_LINKS)
		return false;

	in_conf = mt7927_mlo_resident_bss(dev, mvif, link_id);
	link_sta = mt792x_sta_to_link_sta(vif, sta, link_id);
	if (!in_conf || !link_sta)
		return false;

	for (id = 0; id < IEEE80211_MLD_MAX_NUM_LINKS; id++) {
		struct mt792x_link_sta *m = mt7927_mlo_resident_sta(dev, msta, id);

		if (!m)
			continue;
		resident |= BIT(id);
		band[id] = m->wcid.phy_idx;
		if (m->wcid.link_valid)
			live |= BIT(id);
	}

	donor_id = mt7925_mlo_handover_donor(msta->deflink_id, resident, live, band,
					     IEEE80211_MLD_MAX_NUM_LINKS, link_id,
					     in_conf->mt76.band_idx);
	if (donor_id < 0)
		return false;

	donor = mt7927_mlo_resident_sta(dev, msta, donor_id);
	if (!donor || donor != &msta->deflink || donor->wcid.hw_key_idx == 0xff)
		return false;

	printk(KERN_INFO
	       "MLO_WCID_HANDOVER: wcid=%u from_link=%d to_link=%u band=%u hw_key_idx=%u peer_old=%pM peer_new=%pM\n",
	       donor->wcid.idx, donor_id, link_id, in_conf->mt76.band_idx, donor->wcid.hw_key_idx,
	       mt792x_sta_to_link_sta(vif, sta, donor_id) ? mt792x_sta_to_link_sta(vif, sta, donor_id)->addr : (u8 *)"\0\0\0\0\0\0",
	       link_sta->addr);

	rcu_assign_pointer(msta->link[donor_id], NULL);
	rcu_assign_pointer(msta->link[link_id], donor);
	msta->deflink_id = link_id;
	msta->valid_links &= ~BIT(donor_id);
	mvif->mt7927_mlo.link[donor_id].resident = false;
	mvif->mt7927_mlo.resident_links &= ~BIT(donor_id);
	donor->wcid.link_id = link_id;
	donor->wcid.phy_idx = in_conf->mt76.band_idx;
	donor->pri_link = link_sta;
	donor->wcid.def_wcid = NULL;
	return true;
}

int mt7927_mlo_change_sta_links(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta,
				u16 old_links, u16 new_links)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long add = new_links & ~old_links;
	unsigned long rem = old_links & ~new_links;
	unsigned long admitted_add = 0;
	unsigned long handed_over = 0;
	unsigned long noroc_test_links = 0;
	unsigned long refresh_links;
	struct mt792x_link_sta *failed_mlink = NULL;
	unsigned int link_id;
	int err = 0;

	if (!mt7927_mlo_active_transition(dev, vif, old_links, new_links))
		return -EOPNOTSUPP;

	mt7925_mlo_clear_tid_link(msta, -1);

	mt792x_mutex_acquire(dev);

	/* mt7927_mlo_change_vif_links() is the sole owner of ROC admission
	 * decisions (mt7927_mlo_link_state.roc_admitted /
	 * .admitted_generation); this callback only acts on them. An add-loop
	 * link is eligible here iff it was admitted by that callback for the
	 * transition_generation currently in effect -- never inferred from
	 * new_links, `created`, link_ready, or the transient global
	 * phy->roc_grant (already consumed/reset by the time this runs). The
	 * generation check specifically guards against a stale, delayed
	 * grant/timeout event from an earlier transition authorizing a
	 * link_id that has since been retired and re-added.
	 *
	 * This must be computed and applied BEFORE
	 * mt7927_mlo_refresh_reassigned_sta(): that helper itself flips
	 * wcid.link_valid=true (via a real MCU STA update) for any link in
	 * its input mask whose phy_idx doesn't yet match its mconf's
	 * band_idx -- exactly the state an unadmitted substitute is left in
	 * right after the VIF callback's band reassignment. Gating only the
	 * later add-loop activation below would still let this helper
	 * publish invalid substitute topology to firmware first.
	 */
	for_each_set_bit(link_id, &add, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt7927_mlo_link_state *lstate = &mvif->mt7927_mlo.link[link_id];

		/* A link the AP negotiated but this driver has never admitted
		 * even once (fresh membership, e.g. still-settling initial
		 * association) must get its persistent per-link STA state now
		 * regardless of ROC-admission bookkeeping -- that bookkeeping
		 * exists to gate a steering-driven REACTIVATION of an
		 * already-known dormant link, not first-ever negotiated
		 * membership. Leaving it gated here is exactly why link0 could
		 * stay msta->link[0]==NULL indefinitely after initial
		 * association negotiated it but no steering event happened to
		 * "grow" into it yet.
		 */
		if ((lstate->roc_admitted &&
		     lstate->admitted_generation == mvif->mt7927_mlo.transition_generation) ||
		    !lstate->negotiated)
			admitted_add |= BIT(link_id);

		if (!(admitted_add & BIT(link_id)) &&
		    READ_ONCE(mt7925_test_mt7927_mlo_noroc_admit) &&
		    READ_ONCE(mt7925_test_mt7927_noroc_peer_admit) &&
		    is_mt7927(&dev->mt76) && !lstate->roc_admitted) {
			struct mt792x_bss_conf *tb =
				mt7927_mlo_resident_bss(dev, mvif, link_id);
			struct mt792x_link_sta *tl =
				mt7927_mlo_resident_sta(dev, msta, link_id);

			admitted_add |= BIT(link_id);
			noroc_test_links |= BIT(link_id);
			printk(KERN_INFO
			       "MLO_NOROC_PEER_ADMIT: link_id=%u bss_idx=%d wcid=%d roc_admitted=%d active_links=0x%x valid_links=0x%x survivor=0x%lx held_token=%u held_links=0x%x old=0x%lx new=0x%lx\n",
			       link_id, tb ? (int)tb->mt76.idx : -1,
			       tl ? (int)tl->wcid.idx : -1, lstate->roc_admitted,
			       vif->active_links, vif->valid_links,
			       (unsigned long)(old_links & new_links),
			       dev->phy.mlo_roc.held_token,
			       dev->phy.mlo_roc.held_links,
			       (unsigned long)old_links, (unsigned long)new_links);
		}

		lstate->negotiated = true;
	}

	/* The survivor (new_links & ~add) is never gated here -- its own
	 * live wcid/key/aggregation state is untouched by this transition and
	 * must remain exactly as it already is.
	 */
	refresh_links = (new_links & ~add) | admitted_add;

	err = mt7927_mlo_refresh_reassigned_sta(dev, vif, sta, msta,
						 refresh_links,
						 admitted_add &&
						 is_mt7927(&dev->mt76) &&
						 mt7925_defer_primary_authorize_active());
	if (err)
		goto out;

	for_each_set_bit(link_id, &rem, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_link_sta *mlink;

		if (msta->postkey_pending &&
		    (link_id == msta->postkey_sublink_id ||
		     link_id == msta->postkey_survivor_id)) {
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "POSTKEY_KEEP_DORMANT: reason=link_retired link=%u\n",
				      link_id);
			mt7927_mlo_pending_clear(msta);
		}

		mlink = mt7927_mlo_resident_sta(dev, msta, link_id);
		if (!mlink)
			continue;

		mvif->mt7927_mlo.link[link_id].removing = true;
		mt7927_mlo_deactivate_wcid(dev, mlink);
		if (READ_ONCE(mt7925_test_mt7927_fw_quiesce_removed)) {
			struct ieee80211_link_sta *rem_link_sta =
				mt792x_sta_to_link_sta(vif, sta, link_id);
			struct mt792x_bss_conf *rem_mconf =
				mt7927_mlo_resident_bss(dev, mvif, link_id);
			struct ieee80211_bss_conf *rem_link_conf =
				mt792x_vif_to_bss_conf(vif, link_id);
			int qerr = -EINVAL, berr = -EINVAL;

			if (rem_link_sta)
				qerr = mt7925_mcu_sta_update(dev, rem_link_sta, vif,
							     mlink, false,
							     MT76_STA_INFO_STATE_NONE, NULL);
			if (rem_mconf && rem_link_conf)
				berr = mt7925_mcu_add_bss_info(&dev->phy,
							       rem_mconf->mt76.ctx,
							       rem_link_conf,
							       rem_link_sta, false);
			if (READ_ONCE(mt7925_test_mt7927_fw_quiesce_dev) &&
			    rem_mconf && rem_link_conf) {
				struct mt792x_link_sta *rem_bmc =
					mt7927_mlo_resident_sta(dev, &mvif->sta,
								link_id);
				int derr = rem_bmc ?
					mt76_connac_mcu_uni_add_dev(&dev->mphy,
								    rem_link_conf,
								    &rem_mconf->mt76,
								    &rem_bmc->wcid,
								    false) :
					-EINVAL;

				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "MLO_FW_QUIESCE_DEV: link=%u bss_idx=%u ret=%d\n",
					      link_id, rem_mconf->mt76.idx, derr);
			}
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_FW_QUIESCE_REMOVED: link=%u wcid=%u bss_idx=%d sta_ret=%d bss_ret=%d\n",
				      link_id, mlink->wcid.idx,
				      rem_mconf ? rem_mconf->mt76.idx : -1,
				      qerr, berr);
		}
		if (READ_ONCE(mt7925_mlo_free_retired_bss_idx) &&
		    READ_ONCE(mt7925_test_mt7927_fw_quiesce_removed) &&
		    is_mt7927(&dev->mt76)) {
			struct mt792x_bss_conf *fm = mt7927_mlo_resident_bss(dev, mvif, link_id);

			/* The retired link's firmware BSS was just made inactive
			 * above: let the next link added take its lowest index,
			 * so a three-link association never has to put a live
			 * pair member on BSS index 2.
			 */
			if (fm) {
				dev->mt76.vif_mask = mt7925_mlo_vif_mask_after_retire(dev->mt76.vif_mask,
									      fm->mt76.idx);
				printk(KERN_INFO
				       "MLO_BSS_IDX_FREED: link=%u bss_idx=%u vif_mask=0x%llx\n",
				       link_id, fm->mt76.idx, dev->mt76.vif_mask);
			}
		}
		mt7927_mlo_note_sta(mvif, mlink, false);
		mvif->mt7927_mlo.link[link_id].removing = false;
		mvif->mt7927_mlo.link[link_id].roc_admitted = false;
	}

	for_each_set_bit(link_id, &add, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_link_sta *link_sta;
		struct mt792x_link_sta *mlink;

		if (!(admitted_add & BIT(link_id))) {
			/* Not ROC-admitted: keep the substitute resident but
			 * dormant. Force its WCID invalid and its per-link
			 * traffic-active state false explicitly -- a stale
			 * prior state must never be able to leave it looking
			 * eligible -- without tearing down the resident
			 * object itself.
			 */
			mlink = mt7927_mlo_resident_sta(dev, msta, link_id);
			if (mlink) {
				mlink->wcid.link_valid = false;
				mt7927_mlo_note_sta(mvif, mlink, false);
			}
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_STA_LINK_ADMIT_TRACE: old=0x%x new=0x%x add=0x%lx gen=%u link=%u roc_admitted=%d admitted_gen=%u wcid_valid_before=%d wcid_valid_after=0 action=keep_dormant\n",
				      old_links, new_links, add,
				      mvif->mt7927_mlo.transition_generation, link_id,
				      mvif->mt7927_mlo.link[link_id].roc_admitted,
				      mvif->mt7927_mlo.link[link_id].admitted_generation,
				      mlink ? mlink->wcid.link_valid : -1);
			continue;
		}

		mlink = mt7927_mlo_resident_sta(dev, msta, link_id);
		if (!mlink && mt7927_mlo_try_wcid_handover(dev, vif, sta, msta, mvif, link_id)) {
			handed_over |= BIT(link_id);
			mlink = mt7927_mlo_resident_sta(dev, msta, link_id);
		}
		if (!mlink) {
			err = mt7925_mac_sta_add_links(dev, vif, sta, BIT(link_id));
			if (noroc_test_links & BIT(link_id))
				printk(KERN_INFO "MLO_NOROC_PEER_STAGE: link=%u stage=sta_add_links ret=%d\n",
				       link_id, err);
			if (err)
				goto out;
			mlink = mt7927_mlo_resident_sta(dev, msta, link_id);
			if (!mlink) {
				err = -EINVAL;
				goto out;
			}
		}

		link_sta = mt792x_sta_to_link_sta(vif, sta, link_id);
		if (!link_sta) {
			err = -EINVAL;
			goto out;
		}

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_STA_LINK_ADMIT_TRACE: old=0x%x new=0x%x add=0x%lx gen=%u link=%u roc_admitted=%d admitted_gen=%u wcid_valid_before=%d wcid_valid_after=1 action=activate\n",
			      old_links, new_links, add,
			      mvif->mt7927_mlo.transition_generation, link_id,
			      mvif->mt7927_mlo.link[link_id].roc_admitted,
			      mvif->mt7927_mlo.link[link_id].admitted_generation,
			      mlink->wcid.link_valid);

		mvif->mt7927_mlo.link[link_id].adding = true;

		/* Withhold this freshly-admitted link from ordinary TX
		 * selection through CREATE/RESTORE/AUTHORIZE below and
		 * through the convergence check that follows -- mirrors
		 * mt7927_mlo_refresh_reassigned_sta()'s existing withhold
		 * (see its comment) for exactly the same reason: link_valid
		 * flips true well before there is any evidence the AP/
		 * firmware RX datapath for this brand-new peer is usable.
		 * Proven live (2026-09-13): with no withhold at all, real
		 * ARP + 8 real pings were dispatched to a newly admitted
		 * link0 with rx_pkts_delta=0 for the entire validation
		 * window before steering correctly rolled back.
		 *
		 * Also reset any stale liveness history: this same link_id's
		 * mlink object can be reused across repeated dormant/admit
		 * cycles (mt7927_mlo_resident_sta() above may return an
		 * already-existing object), so a prior admission generation's
		 * RX history must not be able to satisfy this generation's
		 * convergence proof.
		 */
		msta->mlo_link_ready_jiffies[link_id] = 0;
		mlink->tx_liveness_rx_seen = mlink->mlo_valid_rx_packets;
		mlink->tx_liveness_tx_seen = mlink->wcid.stats.tx_packets;
		mlink->tx_liveness_rx_ever_seen = false;
		mlink->tx_liveness_rx_progress_jiffies = 0;
		/* Security-valid RX proof (used by the sole-secure-link
		 * continuity gate) belongs to ONE incarnation: a reused mlink
		 * object must not carry an earlier admission's timestamp.
		 */
		WRITE_ONCE(mlink->mlo_valid_rx_jiffies, 0);

		mlink->wcid.link_valid = true;
		if (noroc_test_links & BIT(link_id))
			printk(KERN_INFO "MLO_NOROC_PEER_STAGE: link=%u stage=peer_wcid wcid=%u published=%d link_valid=1 hw_key_idx=%d\n",
			       link_id, mlink->wcid.idx,
			       !!rcu_access_pointer(dev->mt76.wcid[mlink->wcid.idx]),
			       mlink->wcid.hw_key_idx);
		if (!READ_ONCE(mt7925_test_mt7927_ptk_after_mld) && !(handed_over & BIT(link_id))) {
			err = mt7925_mlo_reconcile_link(dev, vif, sta, mlink,
							link_id);
			if (noroc_test_links & BIT(link_id))
				printk(KERN_INFO "MLO_NOROC_PEER_STAGE: link=%u stage=ptk_reconcile ret=%d hw_key_idx=%d\n",
				       link_id, err, mlink->wcid.hw_key_idx);
			if (err) {
				failed_mlink = mlink;
				goto out_clear_adding;
			}
		}
		err = mt7925_mac_link_sta_assoc(dev, vif, link_sta);
		if (err) {
			failed_mlink = mlink;
			goto out_clear_adding;
		}
		if (noroc_test_links & BIT(link_id))
			printk(KERN_INFO "MLO_NOROC_PEER_STAGE: link=%u stage=link_sta_assoc(STATE_ASSOC+MLD) done wcid=%u\n",
			       link_id, mlink->wcid.idx);
		if (READ_ONCE(mt7925_test_mt7927_ptk_after_mld)) {
			/* TEST-ONLY: first PTK install after STA_REC_MLD membership. */
			err = mt7925_mlo_reconcile_link(dev, vif, sta, mlink,
							link_id);
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_PTK_AFTER_MLD: link=%u wcid=%u hw_key_idx=%d ret=%d\n",
				      link_id, mlink->wcid.idx,
				      mlink->wcid.hw_key_idx, err);
			if (err) {
				failed_mlink = mlink;
				goto out_clear_adding;
			}
		}
		mt7927_mlo_note_sta(mvif, mlink, true);

		/* CREATE -> RESTORE -> AUTHORIZE above completed; now prove
		 * this specific link_id's RX datapath actually works before
		 * arming the readiness gate mt7925_mlo_link_tx_stable()
		 * (mt792x_core.c) checks. Unlike mt7927_mlo_refresh_reassigned_sta(),
		 * whose migrated peer is reused mid-flow so exhausting the
		 * retry budget can safely fall through to the existing
		 * VALIDATE/rollback backstop, a genuinely fresh link/WCID has
		 * never carried any traffic and the deterministic best-link
		 * selector will never route ordinary user data to a link
		 * this gate reports as not-yet-stable -- so this rearm is
		 * gated strictly on real RX proof, never unconditional.
		 */
		{
			struct mt7927_mlo_admit_retry_ctx retry_ctx = {
				.dev = dev,
				.vif = vif,
				.sta = sta,
				.msta = msta,
				.link_sta = link_sta,
				.peer = mlink,
			};
			u32 baseline_rx = mlink->mlo_valid_rx_packets;
			bool rx_progress = false;

			mt7927_mlo_pt_wait_rx(vif, mvif, link_id);
			mt7927_mlo_link_wake_probe(dev, vif, sta, mlink, link_id);
			err = mt7927_mlo_wait_link_rx_ready(mlink, link_id,
							    baseline_rx,
							    "fresh_admission",
							    mt7927_mlo_reauthorize_added_link,
							    &retry_ctx,
							    &rx_progress);
			if (noroc_test_links & BIT(link_id))
				printk(KERN_INFO "MLO_NOROC_PEER_STAGE: link=%u stage=rx_ready_wait ret=%d rx_progress=%d\n",
				       link_id, err, rx_progress);
			if (err) {
				failed_mlink = mlink;
				goto out_clear_adding;
			}

			if (rx_progress) {
				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "MLO_LINK_ADMIT_READY: link=%u rx_progress=1\n",
					      link_id);
				WRITE_ONCE(mlink->mlo_await_valid_rx, false);
				msta->mlo_link_ready_jiffies[link_id] = jiffies;
				mt7927_mlo_pt_rx_ready(vif, mvif, link_id,
						       mlink->mlo_valid_rx_packets);
			} else {
				err = mt7927_mlo_pt_rx_pending(vif, mvif, dev, link_id);
				if (err) {
					/* The replacement never reached secure RX: do not
					 * publish it. mac80211 ignores this callback's error
					 * and keeps the requested mask, so the completion
					 * owner restores the survivor after the setter.
					 */
					WRITE_ONCE(mlink->mlo_await_valid_rx, false);
					mt7925_mlo_completion_admission_failed(vif, old_links,
									       new_links,
									       mvif->mt7927_mlo.pt_old_links,
									       err);
					failed_mlink = mlink;
					goto out_clear_adding;
				}
				/* Firmware-live but unproven: later valid RX in
				 * this generation may arm readiness (mac.c).
				 */
				WRITE_ONCE(mlink->mlo_await_generation,
					   mvif->mt7927_mlo.transition_generation);
				WRITE_ONCE(mlink->mlo_await_valid_rx, true);
				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "MLO_LINK_ADMIT_NOT_READY: link=%u rx_progress=0 await_valid_rx=1 generation=%u\n",
					      link_id,
					      mvif->mt7927_mlo.transition_generation);
			}
		}

		mvif->mt7927_mlo.link[link_id].adding = false;
	}

	msta->valid_links |= refresh_links;
	mvif->mt7927_mlo.traffic_active_links = refresh_links;

	if (mt7925_mlo_security_rebind &&
	    mvif->mt7927_mlo.radio_group_reincarnated &&
	    is_mt7927(&dev->mt76) && hweight16(new_links) == 2)
		mt7927_mlo_security_rebind_pair(dev, vif, sta, msta, new_links);

	/* EXPERIMENTAL (test_mt7927_refresh_survivor_mld, default OFF): A/B
	 * causal probe for whether a survivor link's stale firmware-held
	 * STA_REC_MLD partner table (see mlo_survivor_refresh.h) matters.
	 * Reissues the same STATE_ASSOC,newly=0 authorize call already used
	 * in production elsewhere -- no peer/key/WCID/BA recreation.
	 */
	if (READ_ONCE(mt7925_test_refresh_survivor_mld)) {
		unsigned long survivors =
			mt7927_mlo_survivor_links(old_links, new_links);
		unsigned int slink;

		if (survivors &&
		    !mt7927_mlo_survivor_refresh_ready(vif->active_links,
						       new_links)) {
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_SURVIVOR_MLD_REFRESH_SKIPPED: reason=not_final old_links=0x%x new_links=0x%x vif_active_links=0x%x\n",
				      old_links, new_links, vif->active_links);
			survivors = 0;
		}

		for_each_set_bit(slink, &survivors, IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_link_sta *smlink;
			struct ieee80211_link_sta *slink_sta;
			struct mt792x_bss_conf *smconf;
			int sret;

			smlink = mt7927_mlo_resident_sta(dev, msta, slink);
			smconf = mt7927_mlo_resident_bss(dev, mvif, slink);
			slink_sta = mt792x_sta_to_link_sta(vif, sta, slink);
			if (!smlink || !smconf || !slink_sta || !smlink->wcid.link_valid)
				continue;

			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)) {
				unsigned long other = (unsigned long)new_links &
						      ~BIT(slink);
				unsigned int olink = find_first_bit(&other,
								    IEEE80211_MLD_MAX_NUM_LINKS);
				struct mt792x_link_sta *omlink =
					olink < IEEE80211_MLD_MAX_NUM_LINKS ?
					mt7927_mlo_resident_sta(dev, msta, olink) : NULL;
				struct mt792x_bss_conf *omconf =
					olink < IEEE80211_MLD_MAX_NUM_LINKS ?
					mt7927_mlo_resident_bss(dev, mvif, olink) : NULL;

				printk(KERN_INFO
				      "MLO_SURVIVOR_MLD_REFRESH: old_links=0x%x new_links=0x%x vif_active_links=0x%x survivor_link=%u survivor_wcid=%u survivor_bss_idx=%u survivor_phy=%u serialized_link_num=%d entry0_wlan_id=%u entry0_bss_idx=%u entry1_wlan_id=%d entry1_bss_idx=%d\n",
				      old_links, new_links, vif->active_links,
				      slink, smlink->wcid.idx, smconf->mt76.idx,
				      smlink->wcid.phy_idx,
				      omlink && omconf ? 2 : 1,
				      smlink->wcid.idx, smconf->mt76.idx,
				      omlink && omconf ? (int)omlink->wcid.idx : -1,
				      omlink && omconf ? (int)omconf->mt76.idx : -1);
			}

			sret = mt7925_mcu_sta_authorize(dev, slink_sta, vif, smlink);

			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_SURVIVOR_MLD_REFRESH_DONE: link=%u wcid=%u ret=%d\n",
				      slink, smlink->wcid.idx, sret);
		}
	}

out:
	/* Deferred primary authorize that the partner retouch never consumed
	 * (partner add skipped or failed): authorize now so the primary is
	 * never left in STATE_NONE.
	 */
	if (msta->mlo_defer_auth_links) {
		unsigned long pending = msta->mlo_defer_auth_links;
		unsigned int dlink;

		for_each_set_bit(dlink, &pending, IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_link_sta *dpeer =
				mt7927_mlo_resident_sta(dev, msta, dlink);
			struct ieee80211_link_sta *dlink_sta =
				mt792x_sta_to_link_sta(vif, sta, dlink);
			int dret = -ENODEV;

			if (dpeer && dlink_sta)
				dret = mt7925_mcu_sta_authorize(dev, dlink_sta,
								vif, dpeer);
			msta->mlo_defer_auth_links &= ~BIT(dlink);
			if (READ_ONCE(mt76_mlo_diag_trace) ||
			    READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_PRIMARY_AUTH_DEFERRED_DONE: pri_link=%u pri_wcid=%d via=fallback ret=%d\n",
				       dlink, dpeer ? (int)dpeer->wcid.idx : -1,
				       dret);
		}
	}
	mt792x_mutex_release(dev);
	return err;

out_clear_adding:
	mvif->mt7927_mlo.link[link_id].adding = false;
	if (failed_mlink)
		failed_mlink->wcid.link_valid = false;
	goto out;
}
