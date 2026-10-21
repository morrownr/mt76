// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Sole pair-policy controller; firmware/security execution stays shared. */
#include "pairs.h"
#include "mlo.h"
#include "../mcu.h"
#include "../mlo_steer_validate.h"
#include "../mlo_pair_transition.h"
#include "../mlo_policy_modes.h"
#include "../mlo_secure_shrink.h"
#include <linux/module.h>
#include <linux/delay.h>

/* TEST-ONLY: negotiate a TID-to-link mapping before SHRINKING the active
 * link set. mt7925_mlo_request_active_links() only asked for a negotiated
 * TTLM when the target set contained a link that was not currently usable
 * (i.e. only when ADDING a dormant link). Since dormant_links stays 0
 * unless a TTLM has already been negotiated, that branch was unreachable in
 * practice -- proven live: zero MLO_TTLM_REQUEST in an entire boot. A
 * removal therefore dropped a link locally while the AP kept its downlink
 * TID mapping on it, which matches the observed failure: uplink still ACKed
 * and broadcast RX still decrypting, but unicast downlink gone.
 */
static bool mt7925_test_mt7927_mlo_ttlm_removal;
module_param_named(test_mt7927_mlo_ttlm_removal,
		   mt7925_test_mt7927_mlo_ttlm_removal, bool, 0644);
MODULE_PARM_DESC(test_mt7927_mlo_ttlm_removal,
		 "TEST-ONLY: negotiate TTLM before shrinking the MT7927 MLO active link set");

/* MLO active-link steering: picks the best 2-link pair from measured
 * per-link RSSI and switches via owned wiphy work plus the synchronous
 * ieee80211_set_active_links() result,
 * reusing the existing STA_REC_MLD/firmware-update path -- no new MCU
 * command. See the mlo_link_rssi comment in mt792x.h for the real
 * limitation on which links can have fresh RSSI at any given time.
 */
#define MT7925_MLO_STEER_HYSTERESIS_DB	5
/* Proven live (2026-09-06): 10s was not enough settle time for a substitute
 * link's admission (noroc_admit's ROC-timeout fallback alone takes ~4-5s)
 * before the next steer_work tick was willing to fire another transition.
 * Two switches ~15-20s apart still collapsed the pair to a single valid
 * link -- neither member of the second target pair ended up admitted,
 * despite wpa_state and the driver's own active_links bookkeeping both
 * still reporting healthy. 20s gives ~4x margin over the observed
 * admission time while staying safely under MT7925_MLO_RSSI_STALE_JIFFIES
 * (30s) so a legitimate subsequent switch doesn't get starved of fresh
 * RSSI right as cooldown expires. This does not fix the underlying race
 * (see MLO findings, "single-link collapse under rapid cascades") -- it
 * only reduces how often steer_work can trigger it by giving each
 * transition more time to fully settle before the next is allowed.
 */
#define MT7925_MLO_STEER_COOLDOWN	(20 * HZ)
#define MT7925_MLO_RSSI_STALE_JIFFIES	(30 * HZ)
#define MT7925_MLO_STEER_6G_DEGRADE_THRESHOLD	-75
#define MT7925_MLO_STEER_6G_DEGRADE_PENALTY	30

/* Conservative floor score for a negotiated/resident/keyed dormant link that
 * has never had a single real RSSI sample this association (the hardware
 * RSSI monitor only ever arms one link at a time and can take several
 * rotation ticks to reach it -- see mt7925_mlo_rotate_rssi_monitor()). This
 * must never be confused with S32_MIN (genuinely ineligible: not resident,
 * not valid, or unkeyed) nor be strong enough to beat a real, healthy
 * measured link -- it exists only so an otherwise-eligible substitute isn't
 * permanently unselectable purely for lacking telemetry, which is exactly
 * the bug this exists to fix: without it, every pair needing a never-
 * sampled dormant link scored S32_MIN forever and auto-steering could never
 * choose a substitute at all.
 */
#define MT7925_MLO_STEER_UNKNOWN_SCORE		(-70)
/* Linear staleness penalty for a CACHED (previously measured, now dormant)
 * link's score, capped so a very old sample still beats UNKNOWN outright
 * (a real measurement, however old, is still more informative than none)
 * but is clearly discounted relative to a fresh one.
 */
#define MT7925_MLO_STEER_STALE_PENALTY_PER_SEC	1
#define MT7925_MLO_STEER_STALE_PENALTY_MAX	20
/* How long an automatically-activated exploratory substitute gets before
 * steer_work judges whether to keep it or roll back. Longer than one
 * MT7925_MLO_STEER_INTERVAL tick so the link has at least one real chance
 * to pick up live RSSI/traffic; shorter than MT7925_MLO_STEER_COOLDOWN so a
 * bad substitute doesn't stay committed for the full cooldown period.
 */
#define MT7925_MLO_STEER_VALIDATE_WINDOW	(6 * HZ)
/* A link that just failed exploratory validation is excluded from being
 * picked again for this long, so a persistently-bad dormant candidate
 * doesn't get retried every cooldown cycle indefinitely.
 */
#define MT7925_MLO_STEER_CANDIDATE_COOLDOWN	(60 * HZ)

enum mt7925_mlo_link_metric_state {
	MT7925_MLO_METRIC_INVALID,	/* not resident/valid/keyed: S32_MIN, hard reject */
	MT7925_MLO_METRIC_LIVE,		/* currently active, fresh RSSI */
	MT7925_MLO_METRIC_CACHED,	/* dormant, real sample from before, aged */
	MT7925_MLO_METRIC_UNKNOWN,	/* eligible, never sampled this association */
};

static const char *mt7925_mlo_metric_state_str(enum mt7925_mlo_link_metric_state s)
{
	switch (s) {
	case MT7925_MLO_METRIC_LIVE:
		return "LIVE";
	case MT7925_MLO_METRIC_CACHED:
		return "CACHED";
	case MT7925_MLO_METRIC_UNKNOWN:
		return "UNKNOWN";
	default:
		return "INVALID";
	}
}

/* Classifies link_id's steering usability without ever arming the hardware
 * RSSI monitor here -- that stays exclusively owned by
 * mt7925_mlo_rotate_rssi_monitor()'s existing rotation (untouched by this
 * change, including its dormant-link safety restriction). This only reads
 * whatever mlo_link_rssi[]/mlo_link_rssi_jiffies[] that rotation already
 * populated, which is exactly the per-link steering metric cache the
 * scorer needs -- no new state, no new sampling.
 */
/* Passive per-link signal: the WCID's own RX signal average, fed by every real
 * received frame, exactly what the TX selector already trusts. Counted only
 * while that link's security-valid RX is recent, so a link that stopped
 * delivering cannot keep a stale reading alive. No hardware RSSI monitor is
 * armed for this (arming it on an active link is proven to break its RX, see
 * mt7925_mlo_rotate_rssi_monitor()).
 */
static bool mt7925_mlo_passive_rssi(struct mt792x_link_sta *mlink, s32 *rssi)
{
	unsigned long last;
	s32 v;

	if (!mlink || !mlink->wcid.link_valid)
		return false;
	last = READ_ONCE(mlink->mlo_valid_rx_jiffies);
	if (!last || time_after(jiffies, last + MT7925_MLO_RSSI_STALE_JIFFIES))
		return false;
	v = -(s32)ewma_signal_read(&mlink->wcid.rssi);
	if (!v)
		return false;
	*rssi = v;
	return true;
}

static enum mt7925_mlo_link_metric_state
mt7925_mlo_link_metric(struct mt792x_vif *mvif, u16 current_links,
		       int link_id, struct mt792x_link_sta *mlink,
		       s32 *out_score)
{
	unsigned long age_jiffies, age_sec, penalty;
	s32 passive;

	/* mvif->valid_links (the generic, non-MT7927-specific field) is only
	 * ever written by mt7925_change_vif_links()'s single-link-creation
	 * path, which every real MT7927 pair transition bypasses (see
	 * mt7927_mlo_active_transition()'s dispatch) -- it does not reliably
	 * reflect a link's true negotiated membership on this chip.
	 * mt7927_mlo.negotiated_links is refreshed from mac80211's own
	 * authoritative vif->valid_links on every MT7927 transition
	 * (mt7927_mlo_change_vif_links() entry) -- proven live reliable.
	 *
	 * NOT also requiring mt7927_mlo.resident_links here, still: two
	 * separate backfill fixes were added at both admission-path entry
	 * points this session (mt7927_mlo_change_vif_links() and the generic
	 * mt7925_change_vif_links()) and resident_links now correctly picks
	 * up link1/link2 -- but link0 still showed resident=0 live after
	 * both fixes, on the actual admission path this chip took for it
	 * this association. That remaining gap is real and unresolved; do
	 * not silently enforce an invariant that live evidence still
	 * contradicts. negotiated_links (mac80211's own authoritative view,
	 * proven reliable) plus the later hw_key_idx==255 check remain the
	 * enforced safety net.
	 */
	if (!(mvif->mt7927_mlo.negotiated_links & BIT(link_id))) {
		*out_score = S32_MIN;
		return MT7925_MLO_METRIC_INVALID;
	}

	if ((current_links & BIT(link_id)) &&
	    mt7925_mlo_passive_rssi(mlink, &passive)) {
		*out_score = passive;
		return MT7925_MLO_METRIC_LIVE;
	}

	if (mvif->mlo_link_rssi[link_id] == MT792X_MLO_RSSI_UNKNOWN) {
		*out_score = MT7925_MLO_STEER_UNKNOWN_SCORE;
		return MT7925_MLO_METRIC_UNKNOWN;
	}

	if (current_links & BIT(link_id)) {
		/* Live gate unchanged from before: a currently-active link's
		 * sample older than the stale window is treated as UNKNOWN
		 * rather than trusted, same as the prior no-data behaviour.
		 */
		if (time_after(jiffies, mvif->mlo_link_rssi_jiffies[link_id] +
				       MT7925_MLO_RSSI_STALE_JIFFIES)) {
			*out_score = MT7925_MLO_STEER_UNKNOWN_SCORE;
			return MT7925_MLO_METRIC_UNKNOWN;
		}
		*out_score = mvif->mlo_link_rssi[link_id];
		return MT7925_MLO_METRIC_LIVE;
	}

	/* Dormant, but has a real historical sample: use it with an age
	 * penalty instead of discarding it. A last-known-good dormant link
	 * is a far better candidate than an unmeasured one.
	 */
	age_jiffies = jiffies - mvif->mlo_link_rssi_jiffies[link_id];
	age_sec = jiffies_to_msecs(age_jiffies) / 1000;
	penalty = min_t(unsigned long, age_sec * MT7925_MLO_STEER_STALE_PENALTY_PER_SEC,
			MT7925_MLO_STEER_STALE_PENALTY_MAX);
	*out_score = mvif->mlo_link_rssi[link_id] - (s32)penalty;
	return MT7925_MLO_METRIC_CACHED;
}

/* TEST-ONLY: zero disables the override; negative values replace Link 2's
 * measured RSSI in the steering scorer without changing cached telemetry.
 */
int mt7925_mlo_link2_rssi_override;

/* Band-pair preference bonus: deliberately smaller than a real, common RSSI
 * gap (tens of dB) so it only breaks near-ties between comparably-usable
 * pairs -- it must never make steering prefer a 5GHz link that is actually
 * weak just because it is 5GHz. RSSI stays the primary signal (added into
 * the same score before this bonus, mt7925_mlo_pair_score() below).
 */
#define MT7925_MLO_STEER_5G6G_BONUS	8

/* Steering candidates are policy MODES (mlo_policy_modes.h), not link
 * bitmaps. Each mode resolves to the link bitmap of the bands actually
 * negotiated on this association. The old single-radio 5+6 GHz bitmap is not
 * a candidate: 5 and 6 GHz share firmware slot 1 and have never carried
 * authenticated traffic on both links (Memory 943), so scoring it as a
 * simultaneous pair would steer INTO a state the hardware cannot run.
 */
static const u8 mt7925_mlo_steer_modes[] = {
	MT7925_MLO_MODE_SIM_24_5, MT7925_MLO_MODE_SIM_24_6,
	MT7925_MLO_MODE_EMLSR_5, MT7925_MLO_MODE_EMLSR_6,
};

int mt7925_mlo_band_index(enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return MT7925_MLO_BAND_2G;
	case NL80211_BAND_5GHZ:
		return MT7925_MLO_BAND_5G;
	case NL80211_BAND_6GHZ:
		return MT7925_MLO_BAND_6G;
	default:
		return -1;
	}
}

/* No firmware operation that retunes the shared 5/6 GHz radio has been
 * proven on this chip: UNI 0x59 (SwitchMlsrMode) is not sent by Linux and
 * its payload is unrecovered, and MLSR request types 10/11 leave grants
 * unchanged (Memory 943). Flip this only together with live evidence.
 */
static bool mt7925_mlo_emlsr_switch_primitive_verified(void)
{
	return false;
}

enum nl80211_band mt7925_mlo_link_band(struct ieee80211_vif *vif, int link_id)
{
	struct ieee80211_bss_conf *bss_conf;
	struct ieee80211_channel *chan;

	bss_conf = mt792x_vif_to_bss_conf(vif, link_id);
	if (!bss_conf)
		return NUM_NL80211_BANDS;

	chan = bss_conf->chanreq.oper.chan;
	if (!chan)
		return NUM_NL80211_BANDS;

	return chan->band;
}

static s32 mt7925_mlo_pair_bonus(struct ieee80211_vif *vif, u16 pair)
{
	unsigned long links = pair;
	bool has_5g = false, has_6g = false;
	int link_id;

	for_each_set_bit(link_id, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
		switch (mt7925_mlo_link_band(vif, link_id)) {
		case NL80211_BAND_5GHZ:
			has_5g = true;
			break;
		case NL80211_BAND_6GHZ:
			has_6g = true;
			break;
		default:
			break;
		}
	}

	return (has_5g && has_6g) ? MT7925_MLO_STEER_5G6G_BONUS : 0;
}

/* TEST-ONLY: attempt a transition class that has not completed with
 * authenticated validation in this association. Unverified classes failed
 * destructively when tried live, so normal operation never does this.
 */
static bool mt7925_mlo_steer_try_unverified;
module_param_named(mlo_steer_try_unverified, mt7925_mlo_steer_try_unverified,
		   bool, 0644);
MODULE_PARM_DESC(mlo_steer_try_unverified,
		 "TEST-ONLY: let steering attempt transition classes not yet verified in this association (default N)");

/* Per-association steering state is keyed by the association epoch so a new
 * association starts with nothing verified and nothing reported.
 */
static void mt7925_mlo_steer_epoch_sync(struct mt792x_vif *mvif)
{
	u64 epoch = (u64)READ_ONCE(mvif->mlo_completion.assoc_epoch) + 1;

	if (mvif->mt7927_mlo.steer_state_epoch == epoch)
		return;
	mvif->mt7927_mlo.steer_state_epoch = epoch;
	mvif->mt7927_mlo.verified_switch_classes = 0;
	memset(mvif->mt7927_mlo.mode_avail_noted, 0,
	       sizeof(mvif->mt7927_mlo.mode_avail_noted));
	memset(mvif->mt7927_mlo.mode_block_noted, 0,
	       sizeof(mvif->mt7927_mlo.mode_block_noted));
}

void mt7925_mlo_mode_band_of_link(const struct mt7925_mlo_mode_ctx *c,
				  int band_of_link[3])
{
	int b;

	band_of_link[0] = band_of_link[1] = band_of_link[2] = MT7925_MLO_BAND_2G;
	for (b = 0; b < 3; b++)
		if (c->band_present & BIT(b))
			band_of_link[c->band_link[b]] = b;
}

/* Builds the mode-evaluation context from what this association actually
 * negotiated and advertises. Nothing here is assumed: a band that is not
 * negotiated is absent, the AP's EML capability is the one it advertised in
 * its Multi-Link element, and ours is what the wiphy advertises (initialised
 * from firmware NIC_CAP, so it is also the firmware's own claim).
 */
void mt7925_mlo_build_mode_ctx(struct ieee80211_vif *vif,
			       struct mt792x_vif *mvif,
			       struct mt7925_mlo_mode_ctx *c)
{
	const struct wiphy_iftype_ext_capab *ext_capa;
	unsigned long links = mvif->mt7927_mlo.negotiated_links;
	int link_id, band;

	memset(c, 0, sizeof(*c));
	for_each_set_bit(link_id, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
		band = mt7925_mlo_band_index(mt7925_mlo_link_band(vif, link_id));
		if (band < 0 || (c->band_present & BIT(band)))
			continue;
		c->band_link[band] = link_id;
		c->band_present |= BIT(band);
	}

	c->ap_emlsr = !!(vif->cfg.eml_cap & IEEE80211_EML_CAP_EMLSR_SUPP);
	ext_capa = cfg80211_get_iftype_ext_capa(mvif->phy->mt76->hw->wiphy,
						ieee80211_vif_type_p2p(vif));
	if (ext_capa)
		c->local_emlsr_adv =
			!!(ext_capa->eml_capabilities & IEEE80211_EML_CAP_EMLSR_SUPP);
	/* The firmware's own statement for the non-AP role: support flag AND an
	 * EMLSR capability bit, both from its MLO capability record.
	 */
	c->fw_emlsr = mvif->phy->mlo_fw_cap.valid &&
		      mvif->phy->mlo_fw_cap.nonap_eml_support &&
		      (mvif->phy->mlo_fw_cap.nonap_eml_cap & IEEE80211_EML_CAP_EMLSR_SUPP);
	c->switch_primitive_verified = mt7925_mlo_emlsr_switch_primitive_verified();
}

/* Report a mode's availability once per change, not every 5 s tick. */
static void mt7925_mlo_note_mode_avail(struct mt792x_dev *dev,
				       struct mt792x_vif *mvif, int idx,
				       int mode, int avail, unsigned int links)
{
	u8 tag = avail + 1;

	if (mvif->mt7927_mlo.mode_avail_noted[idx] == tag)
		return;
	mvif->mt7927_mlo.mode_avail_noted[idx] = tag;
	dev_info(dev->mt76.dev,
		 "MLO_STEER_MODE: mode=0x%x available=%d links=0x%x reason=%s\n",
		 mode, avail == MT7925_MLO_MODE_OK, links,
		 mt7925_mlo_mode_reason(avail));
}

/* Returns the RSSI-only score in *rssi_score and the band-pair bonus in
 * *bonus (both S32_MIN if the pair has no usable candidate data), so the
 * caller can trace them separately; the return value is their sum, the
 * actual comparison score.
 */
static s32 mt7925_mlo_pair_score_parts(struct ieee80211_vif *vif,
				       u16 current_links, u16 pair,
				       s32 *rssi_score, s32 *bonus,
				       s32 *anchor_rssi,
				       s32 *degrade_penalty,
				       bool *is_experimental)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_sta *sta;
	struct mt792x_sta *msta;
	int override = READ_ONCE(mt7925_mlo_link2_rssi_override);
	bool anchor_valid, anchor_degraded;
	unsigned long links = pair;
	s32 score = 0, quality = 0;
	int link_id;

	*is_experimental = false;
	rcu_read_lock();
	sta = ieee80211_find_sta(vif, vif->cfg.ap_addr);
	msta = sta ? (struct mt792x_sta *)sta->drv_priv : NULL;

	anchor_valid = (mvif->mt7927_mlo.negotiated_links & BIT(2)) &&
		       (mvif->mt7927_mlo.resident_links & BIT(2));
	if (override) {
		*anchor_rssi = override;
	} else if ((current_links & BIT(2)) &&
		   mt7925_mlo_passive_rssi(mt792x_sta_link_rcu(msta, 2),
					   anchor_rssi)) {
		/* the live 6 GHz link's own signal average: real telemetry,
		 * no hardware monitor armed
		 */
		anchor_valid = true;
	} else if (anchor_valid &&
		   mvif->mlo_link_rssi[2] != MT792X_MLO_RSSI_UNKNOWN &&
		   !time_after(jiffies, mvif->mlo_link_rssi_jiffies[2] +
				       MT7925_MLO_RSSI_STALE_JIFFIES)) {
		*anchor_rssi = mvif->mlo_link_rssi[2];
	} else {
		anchor_valid = false;
		*anchor_rssi = MT792X_MLO_RSSI_UNKNOWN;
	}

	/* The 6 GHz anchor is degraded whenever the CURRENT pair rides on it
	 * (any mode containing link 2), not only in the old 5+6 GHz mask.
	 */
	anchor_degraded = (current_links & BIT(2)) && anchor_valid &&
			  *anchor_rssi < MT7925_MLO_STEER_6G_DEGRADE_THRESHOLD;
	*degrade_penalty = 0;

	for_each_set_bit(link_id, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_link_sta *mlink;
		enum mt7925_mlo_link_metric_state mstate;
		u32 rate_kbps;
		s32 link_score;

		/* A link that just failed exploratory validation stays
		 * excluded from candidacy until its cooldown expires.
		 */
		if (time_before(jiffies, mvif->mlo_steer_link_cooldown[link_id]))
			goto no_data;

		if (link_id == 2 && override) {
			mstate = MT7925_MLO_METRIC_LIVE;
			link_score = override;
		} else {
			mstate = mt7925_mlo_link_metric(mvif, current_links,
							link_id,
							mt792x_sta_link_rcu(msta, link_id),
							&link_score);
		}

		/* A pair containing any UNKNOWN-metric link is EXPERIMENTAL:
		 * its score is still computed (using the fixed placeholder)
		 * so it remains comparable, but the caller must never treat
		 * that placeholder as a real measurement when deciding to
		 * switch -- see the two-tier selection in mt7925_mlo_steer_work().
		 */
		if (mstate == MT7925_MLO_METRIC_UNKNOWN)
			*is_experimental = true;

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_STEER_LINK_METRIC: link=%d state=%s rssi=%d age_ms=%ld resident=%d active=%d valid=%d\n",
			      link_id, mt7925_mlo_metric_state_str(mstate),
			      link_score,
			      mvif->mlo_link_rssi[link_id] == MT792X_MLO_RSSI_UNKNOWN ?
				      -1L : jiffies_to_msecs(jiffies - mvif->mlo_link_rssi_jiffies[link_id]),
			      !!(mvif->mt7927_mlo.resident_links & BIT(link_id)),
			      !!(current_links & BIT(link_id)),
			      !!(mvif->mt7927_mlo.negotiated_links & BIT(link_id)));

		if (mstate == MT7925_MLO_METRIC_INVALID)
			goto no_data;

		score += link_score;

		/* Link-quality term, added alongside RSSI rather than
		 * replacing it: a link with no usable key yet (late-add
		 * reconciliation not finished, or genuinely unready) is not
		 * usable regardless of signal strength, so treat it exactly
		 * like the no-RSSI-data case above rather than scoring it.
		 * A ready link's current PHY rate -- mac80211's own
		 * cfg80211_calculate_bitrate(), the same value link_stats
		 * debugfs already shows -- nudges the score by a small,
		 * capped amount (same rough magnitude as the 5G+6G pair
		 * bonus below), so it can only break a close tie between
		 * comparably-usable pairs and never override a real RSSI
		 * gap. A dormant link's cached rate can be stale too, but
		 * it is still real telemetry from when it was last measured,
		 * unlike a fabricated estimate.
		 *
		 * This gate does NOT apply to an UNKNOWN-state link: proven
		 * live, a link mac80211 has negotiated (part of the MLD) can
		 * still have zero real per-link STA state on this driver
		 * (msta->link[link_id] == NULL, no key, no WCID) if it was
		 * never the initial active pair's member on this 2-path DBDC
		 * radio -- not "dormant with stale state", genuinely never
		 * admitted yet. Requiring mlink/hw_key_idx here for such a
		 * link is backwards: exploring it is exactly what triggers
		 * mt7925_mac_sta_add_links() to create that state for real,
		 * via the same admission path a manual debugfs transition
		 * already uses. Hard-rejecting it here made every UNKNOWN
		 * candidate permanently unselectable regardless of the
		 * UNKNOWN score policy above ever intending it to be.
		 */
		mlink = mt792x_sta_link_rcu(msta, link_id);
		if (mlink && mlink->wcid.hw_key_idx != 255) {
			rate_kbps = cfg80211_calculate_bitrate(&mlink->wcid.rate);
			quality += min_t(u32, rate_kbps / 10000, 10);
		} else if (mstate != MT7925_MLO_METRIC_UNKNOWN) {
			goto no_data;
		}
	}

	if (anchor_degraded && pair == current_links) {
		*degrade_penalty = MT7925_MLO_STEER_6G_DEGRADE_PENALTY;
		score -= *degrade_penalty;
	}

	*rssi_score = score;
	*bonus = mt7925_mlo_pair_bonus(vif, pair) + quality;
	rcu_read_unlock();
	return score + *bonus;

no_data:
	*rssi_score = S32_MIN;
	*bonus = S32_MIN;
	*degrade_penalty = 0;
	rcu_read_unlock();
	return S32_MIN;
}

static void mt7925_mlo_rotate_rssi_monitor(struct mt792x_dev *dev,
					   struct ieee80211_vif *vif);

int mt7925_mlo_request_active_links(struct mt792x_dev *dev,
				    struct ieee80211_vif *vif, u16 links)
{
	u16 unusable = links & ~ieee80211_vif_usable_links(vif);
	int ret;

	mt7927_mlo_pairs_note(vif, links, 0, "request", false);
	ret = mt7927_mlo_pairs_admit(vif, links, false);
	if (ret) {
		mt7927_mlo_pairs_note(vif, links, ret, "pair_admission_refused", false);
		return ret;
	}

	if (is_mt7927(&dev->mt76) && ieee80211_vif_is_mld(vif) &&
	    vif->type == NL80211_IFTYPE_STATION && unusable) {
		struct cfg80211_ttlm_params params = {};

		if (links & ~vif->valid_links)
			return -EINVAL;

		/* DO NOT call ieee80211_request_neg_ttlm() from here. It takes
		 * the wiphy mutex, and one of this function's callers,
		 * mt7925_mac_sta_event(), runs under cfg80211_wiphy_work via
		 * ieee80211_rx_mgmt_assoc_resp() with that mutex already held
		 * -- proven live as an unrecoverable wiphy-wide deadlock
		 * (hung task, wpa_supplicant owning the mutex, hard reboot
		 * required). This branch was unreachable in practice anyway
		 * (dormant_links stays 0 without a prior negotiated TTLM, so
		 * "unusable" was always 0 -- zero occurrences in a full boot),
		 * so skipping the request preserves observed behaviour while
		 * removing the landmine. The negotiated path lives in
		 * mt7925_mlo_request_active_links_negotiated(), reachable only
		 * from debugfs, which holds no wiphy lock.
		 */
		(void)params;
		dev_info(dev->mt76.dev,
			 "MLO_TTLM_REQUEST: links=0x%x valid=0x%x usable=0x%x SKIPPED (unsafe lock context)\n",
			 links, vif->valid_links, ieee80211_vif_usable_links(vif));

	}

	/* Legacy/association requests are foreign to an owned steering ticket. */
	mt7925_mlo_completion_supersede(vif);
	ieee80211_set_active_links_async(vif, links);
	return 0;
}

/* TEST-ONLY experiment knob (default N): lets the sole-secure-link veto be
 * bypassed WITHOUT changing the release shape (unlike mlo_roc_release bit 8,
 * which also switches to release-everything). Used to reproduce the
 * 2026-10-04 topology for one-variable experiments; never a production
 * setting. Every bypass is logged by mt7925_mlo_secure_gate().
 */
static bool mt7925_mlo_secure_shrink_bypass;
module_param_named(mlo_secure_shrink_bypass, mt7925_mlo_secure_shrink_bypass,
		   bool, 0644);
MODULE_PARM_DESC(mlo_secure_shrink_bypass,
		 "TEST-ONLY: bypass the sole-secure-link transition veto (default N)");

/* Links of @mask that currently carry security-valid unicast RX (the
 * mlo_valid_rx_* evidence: decrypted data from the current WCID incarnation,
 * never CM/ICV-failed frames, null frames or raw WCID counters). With
 * @use_since the proof must be newer than @since (rollback confirmation);
 * otherwise it must be within MT7925_MLO_PT_SECURE_RX_FRESH_MS.
 */
static u16 mt7925_mlo_secure_links(struct ieee80211_vif *vif, u16 mask,
				   unsigned long since, bool use_since)
{
	struct ieee80211_sta *sta;
	struct mt792x_sta *msta;
	unsigned long bits = mask;
	unsigned int link_id;
	u16 secure = 0;

	rcu_read_lock();
	sta = ieee80211_find_sta(vif, vif->cfg.ap_addr);
	msta = sta ? (struct mt792x_sta *)sta->drv_priv : NULL;
	for_each_set_bit(link_id, &bits, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_link_sta *mlink = msta ?
			mt792x_sta_link_rcu(msta, link_id) : NULL;
		unsigned long lv;

		if (!mlink || !mlink->wcid.link_valid)
			continue;
		lv = READ_ONCE(mlink->mlo_valid_rx_jiffies);
		if (!lv)
			continue;
		if (use_since ? !time_after(lv, since) :
		    !mt7925_mlo_pt_secure_rx_fresh(1, jiffies_to_msecs(jiffies - lv)))
			continue;
		secure |= BIT(link_id);
	}
	rcu_read_unlock();

	return secure;
}

/* Refuse (before it is requested) a transition whose mandatory mac80211
 * shrink would release the only security-valid link. See
 * mlo_secure_shrink.h; destructive experiments bypass it explicitly.
 */
static int mt7925_mlo_secure_gate(struct mt792x_dev *dev,
				  struct ieee80211_vif *vif, u16 links,
				  const char *who)
{
	u16 old = vif->active_links, secure;
	int v;

	if (!is_mt7927(&dev->mt76) || !ieee80211_vif_is_mld(vif) ||
	    vif->type != NL80211_IFTYPE_STATION || !vif->cfg.assoc ||
	    !old || links == old)
		return 0;

	secure = mt7925_mlo_secure_links(vif, old, 0, false);
	v = mt7925_mlo_secure_transition_verdict(old, links, secure);
	if (mt7925_mlo_secure_verdict_allows(v))
		return 0;

	if (mt7927_mlo_destructive_test_gate() ||
	    READ_ONCE(mt7925_mlo_secure_shrink_bypass)) {
		printk(KERN_INFO
		       "MLO_CONTINUITY_TRANSITION_BYPASSED: who=%s old=0x%x new=0x%x secure=0x%x reason=test_gate bypass_param=%d\n",
		       who, old, links, secure,
		       READ_ONCE(mt7925_mlo_secure_shrink_bypass));
		return 0;
	}

	printk(KERN_INFO
	       "MLO_CONTINUITY_TRANSITION_VETOED: who=%s old=0x%x new=0x%x secure=0x%x survivor=0x%x valid_links=0x%x reason=%s\n",
	       who, old, links, secure, old & links, vif->valid_links,
	       mt7925_mlo_secure_verdict_name(v));
	mt7927_mlo_pairs_note(vif, links, -EBUSY,
			      mt7925_mlo_secure_verdict_name(v), false);
	return -EBUSY;
}

/* TEST-ONLY, debugfs entry point ONLY.
 *
 * ieee80211_request_neg_ttlm() takes the wiphy mutex. mt7925_mac_sta_event()
 * reaches mt7925_mlo_request_active_links() from
 * ieee80211_rx_mgmt_assoc_resp() under cfg80211_wiphy_work, which ALREADY
 * holds that mutex -- calling the TTLM request from there self-deadlocks the
 * whole wiphy (proven live: hung-task with wpa_supplicant owning the mutex,
 * box unrecoverable). So the negotiated-shrink experiment must never be
 * reachable from the sta_event/steer paths; it lives here, called only from
 * the mlo_active_links debugfs setter, which runs in plain process context
 * with no wiphy lock held.
 *
 * Negotiating the map is itself the transition: ieee80211_apply_neg_ttlm()
 * sets active_links to the union of the mapped links and marks the rest
 * dormant, so no separate set_active_links call is needed on success.
 */
int mt7925_mlo_request_active_links_negotiated(struct mt792x_dev *dev,
					       struct ieee80211_vif *vif,
					       u16 links)
{
	int err;

	mt7927_mlo_pairs_note(vif, links, 0, "request", false);
	err = mt7927_mlo_pairs_admit(vif, links, true);
	if (err) {
		mt7927_mlo_pairs_note(vif, links, err, "pair_admission_refused", false);
		return err;
	}
	err = mt7925_mlo_secure_gate(dev, vif, links, "request_negotiated");
	if (err)
		return err;

	if (!READ_ONCE(mt7925_test_mt7927_mlo_ttlm_removal) ||
	    !is_mt7927(&dev->mt76) || !ieee80211_vif_is_mld(vif) ||
	    vif->type != NL80211_IFTYPE_STATION ||
	    !vif->active_links || links == vif->active_links)
		return mt7925_mlo_request_active_links(dev, vif, links);

	if (links & ~vif->valid_links) {
		mt7927_mlo_pairs_note(vif, links, -EINVAL, "target_not_negotiated", false);
		return -EINVAL;
	}

	err = mt7925_mlo_ttlm_request(vif, links);
	if (err)
		mt7927_mlo_pairs_note(vif, links, err, "negotiated_ttlm_request", false);
	dev_info(dev->mt76.dev,
		 "MLO_TTLM_NEG: links=0x%x valid=0x%x active=0x%x dormant=0x%x err=%d\n",
		 links, vif->valid_links, vif->active_links,
		 vif->dormant_links, err);
	if (err)
		return err;

	return 0;
}

/* TEST-ONLY: after mlo_auto_steering triggers exactly one real pair
 * switch, immediately disable auto steering so no second transition can
 * fire before the first has been fully inspected. Default off -- normal
 * production auto steering is unaffected unless this is explicitly set.
 */
static bool mt7925_mlo_steer_single_shot;
module_param_named(mlo_steer_single_shot, mt7925_mlo_steer_single_shot,
		   bool, 0644);
MODULE_PARM_DESC(mlo_steer_single_shot,
		 "TEST-ONLY: disable mlo_auto_steering right after the first real pair switch (default N)");

/* How long to poll for an async ieee80211_set_active_links_async() request
 * (forward switch or rollback) to actually apply before giving up on it.
 * ~3s total, well above every observed real admission latency this session
 * (typically well under 1s), polled coarsely since this is a background
 * workqueue wait, not a hot path.
 */
#define MT7925_MLO_STEER_SWITCH_POLL_MS		200
#define MT7925_MLO_STEER_SWITCH_POLL_COUNT	15

/* Post-switch settle: firmware/queues may still be draining/re-arming
 * immediately after active_links flips, even though the mask itself already
 * matches. Any traffic counted during this window is not trustworthy
 * evidence either way, so it is deliberately excluded from the pass/fail
 * baseline -- see MT7925_MLO_STEER_VERIFY_* below, which only starts
 * counting AFTER this elapses.
 */
#define MT7925_MLO_STEER_SETTLE_MS		1000

/* Post-settle verification: real evidence window. Sampled periodically
 * (not just a single delta at the deadline) so a substitute that moved a
 * few packets right after settling and then silently died cannot pass --
 * MT7925_MLO_STEER_PROGRESS_RECENT_MS below requires progress recent
 * enough at the deadline, not just cumulative.
 */
#define MT7925_MLO_STEER_VERIFY_SAMPLE_MS	1000
#define MT7925_MLO_STEER_VERIFY_SAMPLES	8
#define MT7925_MLO_STEER_VERIFY_MIN_PKTS	3
#define MT7925_MLO_STEER_PROGRESS_RECENT_MS	2000
/* If a substitute carried at least this many TX packets during VERIFY, its
 * BA state is expected to matter for a production commit -- a substitute
 * doing real bulk TX while stuck at ampdu_state==0 is not an acceptable
 * final state (see item 7). A handful of exploratory/control packets does
 * not trigger this requirement; sustained bulk traffic does.
 */
#define MT7925_MLO_STEER_BULK_TX_THRESHOLD	50
/* TX volume beyond which "zero RX at all" stops being "not enough of a
 * chance yet" and becomes real proof of failure (mlo_steer_validate.h).
 * Matches MT7925_MLO_TX_LIVENESS_DEAD_TX_THRESHOLD (mt792x_core.c) --
 * same underlying question (has this link had a real, sustained chance to
 * prove itself), asked at a different layer.
 */
#define MT7925_MLO_STEER_DEAD_TX_THRESHOLD	10

struct mt7925_mlo_steer_counters {
	u64 tx_bytes, rx_bytes;
	u32 tx_packets, rx_packets;
	unsigned long ampdu_state;
};

static void mt7925_mlo_steer_read_counters(struct ieee80211_vif *vif,
					   unsigned int link_id,
					   struct mt7925_mlo_steer_counters *c)
{
	struct ieee80211_sta *sta;
	struct mt792x_sta *msta;
	struct mt792x_link_sta *mlink;

	memset(c, 0, sizeof(*c));
	rcu_read_lock();
	sta = ieee80211_find_sta(vif, vif->cfg.ap_addr);
	msta = sta ? (struct mt792x_sta *)sta->drv_priv : NULL;
	mlink = mt792x_sta_link_rcu(msta, link_id);
	if (!mlink)
		goto out;

	c->tx_bytes = mlink->wcid.stats.tx_bytes;
	c->rx_bytes = mlink->wcid.stats.rx_bytes;
	c->tx_packets = mlink->wcid.stats.tx_packets;
	c->rx_packets = mlink->mlo_valid_rx_packets;
	c->ampdu_state = mlink->wcid.ampdu_state;
out:
	rcu_read_unlock();
}

/* Only the ordinary steering worker uses this tracked transport. A negotiated
 * TTLM request reports local acceptance, not outer completion; do not silently
 * pretend it implements this contract when that test-only mode is enabled.
 */
static int mt7925_mlo_steer_request(struct mt792x_dev *dev,
				  struct ieee80211_vif *vif, u16 target,
				  const struct mt792x_mlo_ticket *predecessor,
				  struct mt792x_mlo_ticket *ticket)
{
	int ret;

	mt7927_mlo_pairs_note(vif, target, 0, "request", false);
	if (READ_ONCE(mt7925_test_mt7927_mlo_ttlm_removal)) {
		mt7927_mlo_pairs_note(vif, target, -EOPNOTSUPP, "tracked_ttlm_unsupported", false);
		return -EOPNOTSUPP;
	}
	/* Recheck administrative admission at execution, not just selection.
	 * A ticketed recovery must still be able to restore its previous pair;
	 * its security and predecessor/epoch checks remain authoritative.
	 */
	if (!predecessor) {
		ret = mt7927_mlo_pairs_admit(vif, target, false);
		if (ret) {
			mt7927_mlo_pairs_note(vif, target, ret,
					     "pair_admission_refused", false);
			return ret;
		}
	}
	ret = mt7925_mlo_secure_gate(dev, vif, target, "steer_request");
	if (ret)
		return ret;
	ret = mt7925_mlo_completion_request_after(vif, target, predecessor, ticket);
	if (ret)
		mt7927_mlo_pairs_note(vif, target, ret, "outer_request_refused", false);
	return ret;
}

/* Bounded ordinary-worker polling never acquires wiphy. If the outer
 * operation remains pending, retain its ticket and resume next tick rather
 * than confirming provisional masks or starting an overlapping rollback.
 */
static void mt7925_mlo_steer_switch_validate(struct mt792x_dev *dev,
					     struct ieee80211_vif *vif,
					     struct mt792x_vif *mvif,
					     u16 prev_pair, u16 pending_pair,
					     const struct mt792x_mlo_ticket *request)
{
	unsigned long new_links = pending_pair & ~prev_pair;
	unsigned long all_pending = pending_pair;
	struct mt7925_mlo_steer_counters verify_base[IEEE80211_MLD_MAX_NUM_LINKS];
	unsigned long last_progress_jiffies[IEEE80211_MLD_MAX_NUM_LINKS];
	int link_id, poll, sample;
	struct mt792x_mlo_result core = {};
	int core_ret = -EINPROGRESS, req_ret;
	struct mt7925_mlo_mode_ctx mctx;
	bool switch_ok = false;
	bool prev_is_mode;
	bool ok;

	if (vif->valid_links != 0x7)
		goto invalidated;
	if (mvif->mlo_steer_rollback_pending)
		goto rollback_wait;

	/* A rollback restores hardware state the chip can run. The previous
	 * mask may not be one (the single-radio 5+6 GHz mask): restoring it
	 * only ever returns to a state with no second working link, and the
	 * planner refuses that re-grow anyway. Such a transition settles on
	 * the surviving authenticated state instead of "rolling back" into it.
	 */
	mt7925_mlo_build_mode_ctx(vif, mvif, &mctx);
	/* A single high-band member is a legitimate prior recovery state,
	 * not an EMLSR mode. Restoring it still uses genuine core completion
	 * and fresh security-valid RX, exactly like restoring a prior pair.
	 */
	prev_is_mode = mt7925_mlo_mode_of_links(prev_pair, &mctx) != 0 ||
		(hweight16(prev_pair) == 1 && !(prev_pair & ~vif->valid_links) &&
		 (mt7925_mlo_link_band(vif, __ffs(prev_pair)) == NL80211_BAND_5GHZ ||
		  mt7925_mlo_link_band(vif, __ffs(prev_pair)) == NL80211_BAND_6GHZ));

	/* Only the returned outer operation can finish SWITCH_PENDING. Target
	 * masks and key markers may be visible while grow/key switching runs.
	 */
	for (poll = 0; poll < MT7925_MLO_STEER_SWITCH_POLL_COUNT; poll++) {
		bool substitute_present = true;

		core_ret = mt7925_mlo_completion_result(vif, request, &core);
		if (core_ret == -ECANCELED || core_ret == -ESTALE)
			goto invalidated;
		if (core_ret == -EINPROGRESS) {
			msleep(MT7925_MLO_STEER_SWITCH_POLL_MS);
			continue;
		}
		core_ret = core.core_ret;
		if (core_ret || core.firmware_error)
			break;

		rcu_read_lock();
		for_each_set_bit(link_id, &all_pending, IEEE80211_MLD_MAX_NUM_LINKS) {
			struct ieee80211_sta *sta =
				ieee80211_find_sta(vif, vif->cfg.ap_addr);
			struct mt792x_sta *msta = sta ?
				(struct mt792x_sta *)sta->drv_priv : NULL;
			struct mt792x_link_sta *mlink = msta ?
				mt792x_sta_link_rcu(msta, link_id) : NULL;

			if (!mlink || !mlink->wcid.link_valid ||
			    mlink->wcid.hw_key_idx == 255) {
				substitute_present = false;
				break;
			}
		}
		rcu_read_unlock();

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_STEER_SWITCH_WAIT: active=0x%x pending=0x%x substitute_present=%d\n",
			      vif->active_links, pending_pair, substitute_present);

		if (core.phase == MT792X_MLO_CORE_DONE &&
		    core.final_active == pending_pair &&
		    vif->active_links == pending_pair &&
		    substitute_present) {
			switch_ok = true;
			break;
		}

		msleep(MT7925_MLO_STEER_SWITCH_POLL_MS);
	}

	if (core.phase != MT792X_MLO_CORE_DONE && core_ret == -EINPROGRESS) {
		dev_info(dev->mt76.dev,
			 "MLO_STEER_CORE_PENDING: cookie=%llu epoch=%llu target=0x%x; no confirmation or overlapping rollback\n",
			 request->cookie, request->assoc_epoch, pending_pair);
		return; /* Next tick resumes this SAME ticket, not a new request. */
	}

	if (!switch_ok) {
		dev_info(dev->mt76.dev,
			 "MLO_STEER_ROLLBACK: from=(never applied) to=0x%x reason=switch_never_confirmed\n",
			 prev_pair);
		/* The forward switch itself never confirmed -- nothing to
		 * validate. If still associated, try to restore prev_pair;
		 * either way there is no substitute state to judge or commit.
		 */
		if (vif->cfg.assoc && !prev_is_mode)
			goto rollback_skipped;
		if (vif->cfg.assoc && mt7925_mlo_recovery_reassoc_pending(vif))
			goto rollback_superseded;
		if (vif->cfg.assoc) {
			req_ret = mt7925_mlo_steer_request(dev, vif, prev_pair,
							 request,
							 &mvif->mlo_steer_ticket);
			if (req_ret)
				goto rollback_refused;
			mvif->mlo_steer_rollback_pending = true;
		}
		goto rollback_wait;
	}

	if (mt7925_mlo_completion_result(vif, request, &core))
		goto invalidated;
	dev_info(dev->mt76.dev,
		 "MLO_STEER_CORE_COMPLETED: cookie=%llu epoch=%llu ret=%d prev_pair=0x%x pending_pair=0x%x firmware_status=unverified authenticated_rx=unverified\n",
		 request->cookie, request->assoc_epoch, core.core_ret,
		 prev_pair, pending_pair);
	for_each_set_bit(link_id, &all_pending, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt7925_mlo_steer_counters c;

		mt7925_mlo_steer_read_counters(vif, link_id, &c);
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_STEER_SWITCH_CONFIRMED: prev_pair=0x%x pending_pair=0x%x link=%u tx_bytes_base=%llu rx_bytes_base=%llu tx_pkts_base=%u rx_pkts_base=%u\n",
			      prev_pair, pending_pair, link_id,
			      c.tx_bytes, c.rx_bytes, c.tx_packets, c.rx_packets);
	}

	/* --- SETTLE: let firmware/queues finish draining/re-arming. Any
	 * traffic during this window is deliberately not evidence either way.
	 */
	msleep(MT7925_MLO_STEER_SETTLE_MS);

	if (mt7925_mlo_completion_result(vif, request, &core)) {
		dev_info(dev->mt76.dev,
			 "MLO_STEER_ROLLBACK: from=0x%x to=0x%x reason=assoc_lost_during_settle\n",
			 pending_pair, prev_pair);
		goto invalidated;
	}

	/* --- VERIFY: take the REAL baseline only now, after settle -- this is
	 * what removes trailing/in-flight pre-transition traffic from the
	 * evidence entirely.
	 */
	for_each_set_bit(link_id, &all_pending, IEEE80211_MLD_MAX_NUM_LINKS) {
		mt7925_mlo_steer_read_counters(vif, link_id, &verify_base[link_id]);
		last_progress_jiffies[link_id] = jiffies;
	}

	for (sample = 0; sample < MT7925_MLO_STEER_VERIFY_SAMPLES; sample++) {
		msleep(MT7925_MLO_STEER_VERIFY_SAMPLE_MS);

		if (mt7925_mlo_completion_result(vif, request, &core))
			goto invalidated;

		for_each_set_bit(link_id, &all_pending, IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt7925_mlo_steer_counters c;
			u32 tx_pkts_delta, rx_pkts_delta;

			mt7925_mlo_steer_read_counters(vif, link_id, &c);
			tx_pkts_delta = c.tx_packets - verify_base[link_id].tx_packets;
			rx_pkts_delta = c.rx_packets - verify_base[link_id].rx_packets;

			if (tx_pkts_delta || rx_pkts_delta)
				last_progress_jiffies[link_id] = jiffies;

			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_STEER_PROGRESS: link=%u tx_pkts_delta=%u rx_pkts_delta=%u last_progress_ms_ago=%u\n",
				      link_id, tx_pkts_delta, rx_pkts_delta,
				      jiffies_to_msecs(jiffies - last_progress_jiffies[link_id]));
		}
	}

	/* --- Decide: commit requires PAIR FORWARDING (at least one member of
	 * pending_pair proven with real, recent, bidirectional traffic right
	 * now) and no member proven FAIL. These are deliberately different
	 * facts -- see mlo_steer_validate.h. A member with zero traffic this
	 * window is DEFERRED, not FAIL: proven live (2026-09-12) that the
	 * deterministic best-link TX selector correctly leaves an
	 * already-live survivor's existing TID pin untouched across a
	 * transition, so a freshly admitted sibling can legitimately see
	 * zero dispatch attempts without being broken -- collapsing that
	 * into FAIL caused a real false-negative rollback whose own reverse
	 * migration then cascaded into a full disconnect/reassociation.
	 */
	if (vif->valid_links != 0x7)
		goto invalidated;
	ok = !mt7925_mlo_completion_result(vif, request, &core) &&
	     !core.core_ret && !core.firmware_error && core.final_active == pending_pair &&
	     vif->active_links == pending_pair &&
	     mt7925_mlo_secure_links(vif, pending_pair,
				     core.finished_jiffies, true) == pending_pair;

	{
	bool any_fail = false;
	bool any_pass = false;

	for_each_set_bit(link_id, &all_pending, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_sta *sta;
		struct mt792x_sta *msta;
		struct mt792x_link_sta *mlink;
		struct mt7925_mlo_steer_counters c;
		bool wcid_valid, key_ok, link_ok, ba_ok;
		enum mt7925_mlo_link_metric_state mstate;
		const char *result_str;
		s32 score;
		u32 tx_pkts_delta, rx_pkts_delta;
		u64 tx_bytes_delta, rx_bytes_delta;
		u32 recent_ms;

		mt7925_mlo_steer_read_counters(vif, link_id, &c);
		rcu_read_lock();
		sta = ieee80211_find_sta(vif, vif->cfg.ap_addr);
		msta = sta ? (struct mt792x_sta *)sta->drv_priv : NULL;
		mlink = mt792x_sta_link_rcu(msta, link_id);
		tx_pkts_delta = c.tx_packets - verify_base[link_id].tx_packets;
		rx_pkts_delta = c.rx_packets - verify_base[link_id].rx_packets;
		tx_bytes_delta = c.tx_bytes - verify_base[link_id].tx_bytes;
		rx_bytes_delta = c.rx_bytes - verify_base[link_id].rx_bytes;
		recent_ms = jiffies_to_msecs(jiffies - last_progress_jiffies[link_id]);

		wcid_valid = mlink && mlink->wcid.link_valid;
		key_ok = mlink && mlink->wcid.hw_key_idx != 255;
		mstate = mlink ? mt7925_mlo_link_metric(mvif, vif->active_links,
							link_id, mlink, &score) :
				 MT7925_MLO_METRIC_INVALID;

		link_ok = wcid_valid && key_ok && mstate != MT7925_MLO_METRIC_INVALID;

		/* Bulk-TX/BA policy: only meaningful for a link that actually
		 * carried substantial TX this VERIFY window. A quiet or
		 * lightly-used link is not held to this bar.
		 */
		ba_ok = true;
		if (tx_pkts_delta >= MT7925_MLO_STEER_BULK_TX_THRESHOLD)
			ba_ok = c.ampdu_state != 0;

		if (!link_ok || !ba_ok) {
			/* Hard structural failure (invalid wcid/key/BA on real
			 * bulk TX) always blocks commit, regardless of
			 * is_new/traffic classification.
			 */
			any_fail = true;
			result_str = "fail";
		} else {
			enum mt7925_mlo_link_validate_result vr =
				mt7925_mlo_classify_link_validation(tx_pkts_delta,
								    rx_pkts_delta,
								    MT7925_MLO_STEER_VERIFY_MIN_PKTS,
								    MT7925_MLO_STEER_DEAD_TX_THRESHOLD,
								    recent_ms,
								    MT7925_MLO_STEER_PROGRESS_RECENT_MS);

			/* A link the independent RSSI-history metric already
			 * trusts as LIVE is proven regardless of this
			 * window's traffic -- classification exists to prove
			 * an unproven/new link, not to re-litigate an
			 * already-established survivor every VERIFY tick.
			 */
			if (mstate == MT7925_MLO_METRIC_LIVE)
				vr = MT7925_LINK_VALIDATE_PASS;

			if (vr == MT7925_LINK_VALIDATE_FAIL) {
				any_fail = true;
				result_str = "fail";
			} else if (vr == MT7925_LINK_VALIDATE_PASS) {
				any_pass = true;
				result_str = "pass";
			} else {
				result_str = "deferred";
			}
		}

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_STEER_VALIDATE: prev_pair=0x%x pending_pair=0x%x link=%d metric_state=%s metric=%d tx_bytes_delta=%llu rx_bytes_delta=%llu tx_pkts_delta=%u rx_pkts_delta=%u ba_state=0x%lx wcid_valid=%d key_ok=%d assoc=%d carrier=%d result=%s\n",
			      prev_pair, pending_pair, link_id,
			      mt7925_mlo_metric_state_str(mstate), score,
			      tx_bytes_delta, rx_bytes_delta, tx_pkts_delta, rx_pkts_delta,
			      mlink ? mlink->wcid.ampdu_state : 0UL,
			      wcid_valid, key_ok, vif->cfg.assoc,
			      vif->cfg.assoc, result_str);
		rcu_read_unlock();
	}

	ok = ok && mt7925_mlo_pair_validate_ok(any_fail, any_pass);
	}

	if (ok && mt7925_mlo_completion_validate(vif, request,
			mt7925_mlo_secure_links(vif, pending_pair,
						core.finished_jiffies, true))) {
		dev_info(dev->mt76.dev,
			 "MLO_STEER_COMMIT: cookie=%llu epoch=%llu pair=0x%x reason=authenticated_validation_passed firmware_status=unverified\n",
			 request->cookie, request->assoc_epoch, pending_pair);
		mvif->mlo_steer_validating = false;
		mt7927_mlo_pairs_note(vif, pending_pair, 0, "commit", true);
		{
			int bol[3];

			mt7925_mlo_steer_epoch_sync(mvif);
			mt7925_mlo_mode_band_of_link(&mctx, bol);
			mvif->mt7927_mlo.verified_switch_classes |=
				BIT(mt7925_mlo_switch_class(prev_pair, pending_pair, bol));
		}
		return;
	}

	dev_info(dev->mt76.dev,
		 "MLO_STEER_ROLLBACK: from=0x%x to=0x%x reason=validation_failed\n",
		 pending_pair, prev_pair);
	mt7927_mlo_pairs_note(vif, pending_pair, -EIO, "validation_failed", false);
	if (mt7925_mlo_completion_result(vif, request, &core))
		goto invalidated;
	if (!prev_is_mode)
		goto rollback_skipped;
	if (mt7925_mlo_recovery_reassoc_pending(vif))
		goto rollback_superseded;
	req_ret = mt7925_mlo_steer_request(dev, vif, prev_pair,
					 request,
					 &mvif->mlo_steer_ticket);
	if (req_ret)
		goto rollback_refused;
	mvif->mlo_steer_rollback_pending = true;

rollback_wait:
	for_each_set_bit(link_id, &new_links, IEEE80211_MLD_MAX_NUM_LINKS)
		mvif->mlo_steer_link_cooldown[link_id] =
			jiffies + MT7925_MLO_STEER_CANDIDATE_COOLDOWN;

	for (poll = 0; poll < MT7925_MLO_STEER_SWITCH_POLL_COUNT; poll++) {
		core_ret = mt7925_mlo_completion_result(vif,
					 &mvif->mlo_steer_ticket, &core);
		if (core_ret == -ECANCELED || core_ret == -ESTALE)
			goto invalidated;
		if (core_ret != -EINPROGRESS) {
			if (!core_ret)
				core_ret = core.core_ret;
			break;
		}
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_STEER_ROLLBACK_WAIT: active=0x%x want=0x%x\n",
			      vif->active_links, prev_pair);
		msleep(MT7925_MLO_STEER_SWITCH_POLL_MS);
	}

	if (core.phase != MT792X_MLO_CORE_DONE && core_ret == -EINPROGRESS) {
		dev_info(dev->mt76.dev,
			 "MLO_STEER_ROLLBACK_CORE_PENDING: cookie=%llu epoch=%llu; no confirmation\n",
			 mvif->mlo_steer_ticket.cookie, mvif->mlo_steer_ticket.assoc_epoch);
		return;
	}
	if (core_ret || core.firmware_error || core.final_active != prev_pair) {
		dev_warn(dev->mt76.dev,
			 "MLO_STEER_ROLLBACK_FAILED: core_ret=%d callback_error=%d final=0x%x want=0x%x\n",
			 core_ret, core.firmware_error, core.final_active, prev_pair);
		goto finished;
	}

	/* Core completion and a current association/ticket are prerequisites.
	 * Fresh survivor RX while grow is RUNNING cannot enter this block.
	 * Require each restored member after completion, not just one survivor.
	 */
	if (vif->cfg.assoc && vif->active_links == prev_pair) {
		u16 restored = 0;

		for (poll = 0; poll < MT7925_MLO_STEER_SWITCH_POLL_COUNT; poll++) {
			restored = mt7925_mlo_secure_links(vif, prev_pair,
							   core.finished_jiffies, true);
			if (restored == prev_pair ||
			    mt7925_mlo_completion_result(vif, &mvif->mlo_steer_ticket, &core))
				break;
			msleep(MT7925_MLO_STEER_SWITCH_POLL_MS);
		}

		if (!mt7925_mlo_completion_result(vif, &mvif->mlo_steer_ticket, &core) &&
		    !core.core_ret && !core.firmware_error && vif->active_links == prev_pair &&
		    restored == prev_pair &&
		    mt7925_mlo_completion_validate(vif, &mvif->mlo_steer_ticket, restored))
			dev_info(dev->mt76.dev,
				 "MLO_STEER_ROLLBACK_CONFIRMED: pair=0x%x secure_rx_links=0x%x\n",
				 prev_pair, restored);
		else if (vif->cfg.assoc)
			dev_warn(dev->mt76.dev,
				 "MLO_STEER_ROLLBACK_UNVERIFIED: active=0x%x want=0x%x secure_rx_since_request=0x%x; mask restored but no security-valid RX proves the datapath\n",
				 vif->active_links, prev_pair, restored);
	} else if (vif->cfg.assoc)
		dev_warn(dev->mt76.dev,
			 "MLO_STEER_ROLLBACK_TIMEOUT: active=0x%x want=0x%x\n",
			 vif->active_links, prev_pair);
finished:
	mvif->mlo_steer_validating = false;
	mvif->mlo_steer_rollback_pending = false;
	return;
rollback_skipped:
	dev_info(dev->mt76.dev,
		 "MLO_STEER_ROLLBACK_SKIPPED: prev=0x%x is not a steering mode; settled on active=0x%x secure=0x%x\n",
		 prev_pair, vif->active_links,
		 mt7925_mlo_secure_links(vif, vif->active_links, 0, false));
	for_each_set_bit(link_id, &new_links, IEEE80211_MLD_MAX_NUM_LINKS)
		mvif->mlo_steer_link_cooldown[link_id] =
			jiffies + MT7925_MLO_STEER_CANDIDATE_COOLDOWN;
	goto finished;
rollback_superseded:
	dev_info(dev->mt76.dev,
		 "MLO_STEER_ROLLBACK_SUPERSEDED: prev=0x%x; a reassociation was requested, the replacement is decided at the new association\n",
		 prev_pair);
	for_each_set_bit(link_id, &new_links, IEEE80211_MLD_MAX_NUM_LINKS)
		mvif->mlo_steer_link_cooldown[link_id] =
			jiffies + MT7925_MLO_STEER_CANDIDATE_COOLDOWN;
	goto finished;
rollback_refused:
	dev_warn(dev->mt76.dev, "MLO_STEER_ROLLBACK_REFUSED: want=0x%x err=%d\n",
		 prev_pair, req_ret);
	goto finished;
invalidated:
	dev_info(dev->mt76.dev, "MLO_STEER_INVALIDATED: request/association changed; no confirmation\n");
	goto finished;
}

/* Single-active-link MLO recovery is not an EMLSR operating mode. Only an
 * already active, freshly security-valid high-band member may survive.
 */
static u16 mt7925_mlo_singleton_fallback(u16 active, u16 secure,
				       const struct mt7925_mlo_mode_ctx *c)
{
	int band;

	for (band = MT7925_MLO_BAND_5G; band <= MT7925_MLO_BAND_6G; band++) {
		u16 member;

		if (!(c->band_present & BIT(band)))
			continue;
		member = BIT(c->band_link[band]);
		if ((active & secure & member) && active != member)
			return member;
	}
	return 0;
}

void mt7925_mlo_steer_work(struct work_struct *work)
{
	struct mt792x_vif *mvif = container_of(to_delayed_work(work),
					       struct mt792x_vif,
					       mlo_steer_work);
	struct ieee80211_vif *vif =
		container_of((void *)mvif, struct ieee80211_vif, drv_priv);
	struct mt792x_dev *dev = mvif->phy->dev;
	u16 current_links = vif->active_links;
	u16 best_pair = current_links;
	u16 best_experimental_pair = 0;
	u16 selected_pair;
	s32 best_score = S32_MIN;
	s32 best_experimental_score = S32_MIN;
	s32 cur_score, cur_degrade_penalty = 0;
	const char *selected_reason;
	u16 secure_now = 0;
	struct mt7925_mlo_mode_ctx mctx;
	int band_of_link[3];
	bool cur_is_mode;
	int i, req_err;

	if (!READ_ONCE(mvif->mlo_completion.enabled))
		return;
	if (mvif->mlo_steer_validating) {
		mt7925_mlo_steer_switch_validate(dev, vif, mvif,
					 mvif->mlo_steer_prev_pair,
					 mvif->mlo_steer_pending_pair,
					 &mvif->mlo_steer_ticket);
		goto requeue;
	}
	if (!mt7927_mlo_pairs_can_steer(vif)) {
		WRITE_ONCE(mvif->mlo_auto_steering, false);
		goto requeue;
	}
	if (!mvif->mlo_auto_steering || !vif->cfg.assoc ||
	    !ieee80211_vif_is_mld(vif))
		goto requeue;

	/* Policy V2 selects an operational subset; it never substitutes for
	 * genuine three-link membership. Keep completion reconciliation above
	 * this gate so a lost membership cannot strand an outstanding ticket.
	 * A valid7 singleton survivor remains eligible for secure recovery.
	 */
	mt7925_mlo_build_mode_ctx(vif, mvif, &mctx);
	if (vif->valid_links != 0x7 ||
	    mvif->mt7927_mlo.negotiated_links != vif->valid_links ||
	    mctx.band_present != (BIT(MT7925_MLO_BAND_2G) |
				  BIT(MT7925_MLO_BAND_5G) |
				  BIT(MT7925_MLO_BAND_6G)))
		goto requeue;

	mt7925_mlo_rotate_rssi_monitor(dev, vif);

	secure_now = mt7925_mlo_secure_links(vif, current_links, 0, false);

	{
		s32 rssi_ignored, bonus_ignored, anchor_ignored;
		bool experimental_ignored;

		cur_score = mt7925_mlo_pair_score_parts(vif, current_links,
							current_links, &rssi_ignored,
							&bonus_ignored, &anchor_ignored,
							&cur_degrade_penalty,
							&experimental_ignored);
	}

	mt7925_mlo_steer_epoch_sync(mvif);
	mt7925_mlo_mode_band_of_link(&mctx, band_of_link);
	cur_is_mode = mt7925_mlo_mode_of_links(current_links, &mctx) != 0;

	for (i = 0; i < ARRAY_SIZE(mt7925_mlo_steer_modes); i++) {
		int mode = mt7925_mlo_steer_modes[i];
		unsigned int mode_links;
		int avail = mt7925_mlo_mode_eval(mode, &mctx, &mode_links);
		u16 pair = mode_links;
		s32 rssi_score, bonus, score, anchor_rssi, degrade_penalty;
		bool experimental;
		unsigned long link_bits = pair;
		int link_id;

		mt7925_mlo_note_mode_avail(dev, mvif, i, mode, avail, mode_links);
		if (avail != MT7925_MLO_MODE_OK)
			continue;
		if (!mt7927_mlo_pair_enabled(vif, pair))
			continue;
		/* A target that just failed a real transition waits out its
		 * own backoff; the other mode stays selectable meanwhile.
		 */
		if (pair != current_links &&
		    mt7927_mlo_pair_backoff_active(vif, pair)) {
			if (READ_ONCE(mt76_mlo_diag_trace) ||
			    READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_STEER_BACKOFF: mode=0x%x links=0x%x skipped\n",
				       mode, pair);
			continue;
		}

		/* A band class that needed a firmware reset stays avoided, even when
		 * the AP renumbered the links after it; the others remain options.
		 */
		if (pair != current_links &&
		    mt7925_mlo_severity_blocked(vif, pair, band_of_link)) {
			if (READ_ONCE(mt76_mlo_diag_trace) ||
			    READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_STEER_SEVERE: mode=0x%x links=0x%x skipped\n",
				       mode, pair);
			continue;
		}

		if (pair != current_links) {
			unsigned int cls = mt7925_mlo_switch_class(current_links, pair,
								   band_of_link);

			if (!mt7925_mlo_switch_feasible(cls,
					mvif->mt7927_mlo.verified_switch_classes,
					READ_ONCE(mt7925_mlo_steer_try_unverified))) {
				if (mvif->mt7927_mlo.mode_block_noted[i] != cls + 1) {
					mvif->mt7927_mlo.mode_block_noted[i] = cls + 1;
					dev_info(dev->mt76.dev,
						 "MLO_STEER_MODE_BLOCKED: mode=0x%x links=0x%x current=0x%x class=%s reason=class_not_verified_in_this_association\n",
						 mode, pair, current_links,
						 mt7925_mlo_switch_class_name(cls));
				}
				continue;
			}
		}

		score = mt7925_mlo_pair_score_parts(vif, current_links, pair,
						     &rssi_score, &bonus,
						     &anchor_rssi,
						     &degrade_penalty,
						     &experimental);

		dev_info(dev->mt76.dev,
			 "MLO_STEER: candidate=0x%x anchor_rssi=%d override=%d degrade_penalty=%d\n",
			 pair, anchor_rssi,
			 READ_ONCE(mt7925_mlo_link2_rssi_override),
			 degrade_penalty);
		for_each_set_bit(link_id, &link_bits, IEEE80211_MLD_MAX_NUM_LINKS) {
			enum nl80211_band band = mt7925_mlo_link_band(vif, link_id);
			const char *band_str = band == NL80211_BAND_2GHZ ? "2.4GHz" :
						band == NL80211_BAND_5GHZ ? "5GHz" :
						band == NL80211_BAND_6GHZ ? "6GHz" : "unknown";

			dev_info(dev->mt76.dev, "  link%d band=%s\n", link_id, band_str);
		}
		if (score == S32_MIN)
			dev_info(dev->mt76.dev,
				"  rssi_score=no-data pair_bonus=no-data final_score=no-data\n");
		else
			dev_info(dev->mt76.dev,
				"  rssi_score=%d pair_bonus=%d final_score=%d\n",
				rssi_score, bonus, score);

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_STEER_CANDIDATE: pair=0x%x score=%d eligible=%d experimental=%d reason=%s\n",
			      pair, score, score != S32_MIN, experimental,
			      score == S32_MIN ? "ineligible_link" :
			      pair == current_links ? "current" :
			      experimental ? "unknown_exploration" : "candidate");

		/* Two-tier selection: a pair containing an UNKNOWN-metric
		 * link (experimental) must never win on raw score against a
		 * fully-measured pair -- its placeholder score is not a real
		 * measurement (see mt7925_mlo_link_metric()). It is only
		 * ever a fallback candidate, considered below solely when
		 * the current pair is genuinely degraded and no measured
		 * alternative solves that.
		 */
		if (score == S32_MIN)
			continue;
		/* Never select a pair whose mandatory shrink would release the
		 * only security-valid link (2026-10-04: 0x6->0x3 released the
		 * sole secure L2 for a survivor with no secure RX).
		 */
		if (pair != current_links &&
		    !mt7925_mlo_secure_verdict_allows(
			    mt7925_mlo_secure_transition_verdict(current_links,
								 pair,
								 secure_now)) &&
		    !mt7927_mlo_destructive_test_gate() &&
		    !READ_ONCE(mt7925_mlo_secure_shrink_bypass)) {
			dev_info(dev->mt76.dev,
				 "MLO_STEER_VETO: candidate=0x%x current=0x%x secure=0x%x survivor=0x%x reason=secure_link_would_depart\n",
				 pair, current_links, secure_now,
				 current_links & pair);
			continue;
		}
		if (experimental) {
			if (score > best_experimental_score) {
				best_experimental_score = score;
				best_experimental_pair = pair;
			}
		} else if (score > best_score) {
			best_score = score;
			best_pair = pair;
		}
	}

	dev_info(dev->mt76.dev, "MLO_STEER: selected=0x%x\n", best_pair);

	dev_info(dev->mt76.dev, "MLO_STEER:\n");
	dev_info(dev->mt76.dev, "current=0x%x\n", current_links);
	if (best_pair != current_links)
		dev_info(dev->mt76.dev, "candidate=0x%x\n", best_pair);
	if (cur_score == S32_MIN)
		dev_info(dev->mt76.dev, "old_score=no-data\n");
	else
		dev_info(dev->mt76.dev, "old_score=%d\n", cur_score);
	if (best_score == S32_MIN)
		dev_info(dev->mt76.dev, "new_score=no-data\n");
	else
		dev_info(dev->mt76.dev, "new_score=%d\n", best_score);

	selected_pair = 0;
	selected_reason = NULL;

	/* A current mask that is not a steering mode (the single-radio 5+6
	 * GHz mask, three links, one link) has no comparable score: leave it
	 * for the best measured mode, still under cooldown, rather than
	 * letting a meaningless "current" score block or pin it.
	 */
	if (best_pair != current_links &&
	    best_score != S32_MIN &&
	    (!cur_is_mode ||
	     (cur_score != S32_MIN &&
	      best_score >= cur_score + MT7925_MLO_STEER_HYSTERESIS_DB)) &&
	    time_after(jiffies, mvif->mlo_last_switch_jiffies +
			       MT7925_MLO_STEER_COOLDOWN)) {
		selected_pair = best_pair;
		selected_reason = "better_score";
	} else if ((cur_degrade_penalty > 0 || !cur_is_mode) &&
		   best_experimental_pair &&
		   best_experimental_pair != current_links &&
		   time_after(jiffies, mvif->mlo_last_switch_jiffies +
				      MT7925_MLO_STEER_COOLDOWN)) {
		/* Current pair is genuinely degraded and no fully-measured
		 * candidate solved it -- explicit, narrow exploration of an
		 * UNKNOWN-containing pair, never disguised as a normal
		 * better-score win.
		 */
		selected_pair = best_experimental_pair;
		selected_reason = "unknown_exploration";
	}

	/* With no healthy measured pair, prefer a secure high-band survivor
	 * over exploratory admission. Reuse the verified class gate and the
	 * same request/completion/post-transition security validator below.
	 */
	if ((best_score == S32_MIN || best_pair == current_links) &&
	    (cur_degrade_penalty > 0 || cur_score == S32_MIN) &&
	    time_after(jiffies, mvif->mlo_last_switch_jiffies +
			       MT7925_MLO_STEER_COOLDOWN)) {
		u16 fallback = mt7925_mlo_singleton_fallback(current_links,
							 secure_now, &mctx);

		/* The survivor must be the association anchor: a single high-band
		 * link that gives up the anchor (the stationary 2.4 GHz link of a
		 * 2.4 GHz-anchored pair) is the anchor-excluded class the firmware
		 * cannot carry, and was observed to fire right after a
		 * reassociation, before any RSSI data existed.
		 */
		if (fallback && (fallback & BIT(mvif->deflink_id)) &&
		    !mt7925_mlo_severity_blocked(vif, fallback, band_of_link) &&
		    mt7925_mlo_switch_feasible(
			mt7925_mlo_switch_class(current_links, fallback,
					       band_of_link),
			mvif->mt7927_mlo.verified_switch_classes,
			READ_ONCE(mt7925_mlo_steer_try_unverified))) {
			selected_pair = fallback;
			selected_reason = "secure_highband_singleton";
		}
	}

	if (selected_pair) {
		dev_info(dev->mt76.dev,
			"MLO_STEER: switching=0x%x reason=%s\n",
			selected_pair, selected_reason);
		mvif->mlo_last_switch_jiffies = jiffies;
		dev_info(dev->mt76.dev,
			 "MLO_STEER: mt7925_mlo_request_active_links(0x%x)\n",
			 selected_pair);

		req_err = mt7925_mlo_steer_request(dev, vif, selected_pair,
						 NULL,
						 &mvif->mlo_steer_ticket);
		if (req_err) {
			dev_info(dev->mt76.dev,
				 "MLO_STEER_REQUEST_REFUSED: to=0x%x err=%d; no transition started, nothing to validate\n",
				 selected_pair, req_err);
			goto requeue;
		}

		/* The ordinary worker polls a separate owned wiphy operation.
		 * A bounded pending interval preserves this ticket for the next
		 * tick, never confirms a provisional mask or overlaps rollback.
		 * Cancellation closes the producer gate before joining us; do
		 * not acquire wiphy_lock anywhere in this ordinary worker.
		 */
		mvif->mlo_steer_validating = true;
		mvif->mlo_steer_rollback_pending = false;
		mvif->mlo_steer_prev_pair = current_links;
		mvif->mlo_steer_pending_pair = selected_pair;
		mt7925_mlo_steer_switch_validate(dev, vif, mvif,
						 current_links, selected_pair,
						 &mvif->mlo_steer_ticket);

		if (READ_ONCE(mt7925_mlo_steer_single_shot)) {
			mvif->mlo_auto_steering = false;
			dev_info(dev->mt76.dev,
				 "MLO_STEER_SINGLE_SHOT: auto steering disabled after switching=0x%x\n",
				 selected_pair);
		}
	}

requeue:
	mt7927_mlo_steering_kick(vif);
}

void mt7927_mlo_steering_init(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;

	WRITE_ONCE(mvif->mlo_auto_steering, false);
	INIT_DELAYED_WORK(&mvif->mlo_steer_work, mt7925_mlo_steer_work);
}

/* Firmware's monitor event has no link/BSS identifier. Only the subsystem
 * that armed the one monitor can attribute it; generic CQM stays in MCU.
 */
void mt7927_mlo_steering_rssi_event(struct ieee80211_vif *vif, s32 rssi)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_dev *dev = mvif->phy->dev;
	u8 target;
	int id;

	if (!ieee80211_vif_is_mld(vif) || !mvif->mlo_rssi_monitor_enabled)
		return;
	target = mvif->mlo_rssi_monitor_link_id;
	if (target >= IEEE80211_MLD_MAX_NUM_LINKS)
		return;
	mvif->mlo_link_rssi[target] = clamp_t(s32, rssi, S8_MIN, S8_MAX);
	mvif->mlo_link_rssi_jiffies[target] = jiffies;
	dev_info(dev->mt76.dev, "MLO_STEER_RSSI:\n");
	for (id = 0; id < IEEE80211_MLD_MAX_NUM_LINKS; id++) {
		if (!(mvif->valid_links & BIT(id)))
			continue;
		if (mvif->mlo_link_rssi[id] == MT792X_MLO_RSSI_UNKNOWN)
			dev_info(dev->mt76.dev, "link%d=no-data\n", id);
		else
			dev_info(dev->mt76.dev, "link%d=%d\n", id,
				 mvif->mlo_link_rssi[id]);
	}
}

/* The sole scheduler: lifecycle events only kick this controller. Disabled
 * devices do not poll. An already-owned validation/rollback keeps its ticket.
 * No wiphy acquisition or wait is allowed from the ordinary worker.
 */
void mt7927_mlo_steering_kick(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;

	if (!is_mt7927(&mvif->phy->dev->mt76) || !vif->cfg.assoc ||
	    !ieee80211_vif_is_mld(vif) ||
	    !READ_ONCE(mvif->mlo_completion.enabled))
		return;
	mt7927_mlo_pairs_autostart(vif);
	if (!READ_ONCE(mvif->mlo_steer_validating) &&
	    (!READ_ONCE(mvif->mlo_auto_steering) ||
	     !mt7927_mlo_pairs_can_steer(vif)))
		return;
	ieee80211_queue_delayed_work(mt76_hw(mvif->phy->dev),
				     &mvif->mlo_steer_work,
				     mt7927_mlo_steering_interval(vif));
}

/* Disarms whichever link the MLO-steering RSSI monitor currently targets.
 * Bypasses cfg80211 CQM entirely -- this is a driver-internal arm, not a
 * userspace one, and works whether or not userspace ever configures CQM.
 */
static void mt7925_mlo_disarm_rssi_monitor(struct mt792x_dev *dev,
					   struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_bss_conf *link_conf;

	if (!mvif->mlo_rssi_monitor_enabled)
		return;

	link_conf = mt792x_vif_to_bss_conf(vif, mvif->mlo_rssi_monitor_link_id);
	if (link_conf)
		mt7925_mcu_set_mlo_rssi_monitor(dev, link_conf, false);
	mvif->mlo_rssi_monitor_enabled = false;
}

/* Advances the MLO-steering RSSI monitor to the next valid link (round
 * robin), so every link's RSSI gets refreshed in turn -- only one link's
 * monitor can be armed at a time (see mt7925_mcu_set_mlo_rssi_monitor()).
 * Called once per mt7925_mlo_steer_work() tick while steering is enabled.
 */
static void mt7925_mlo_rotate_rssi_monitor(struct mt792x_dev *dev,
					   struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_bss_conf *link_conf;
	int link_id = mvif->mlo_rssi_monitor_link_id;
	int i, err;

	/* Proven live (first finding): arming the hardware RSSI monitor on a
	 * dormant link (never-admitted, no traffic -- see
	 * mt7927_mlo_prime_dormant_link()) disconnects the AP within ~1s even
	 * though the two REAL active links keep running fine right up to that
	 * point. MT7927 is a genuine 2-path DBDC radio (see
	 * mt7925_mcu_set_mlo_roc()'s fixed 2-element roc[] array) -- sampling
	 * a 3rd band's RSSI while 2 bands are actively serving traffic
	 * apparently steals baseband resources from the real links long
	 * enough to look like beacon loss.
	 *
	 * Second finding, same mechanism, smaller blast radius: arming it on
	 * a link that IS in vif->active_links has the identical effect in
	 * miniature -- live trace showed every single rotation tick on an
	 * active link followed 1-3s later by a burst of RX frames on that
	 * link's channel reporting the WLAN_IDX=4095 invalid-peer sentinel,
	 * with zero mt7927_mlo_reassign_band() or any other transition
	 * running in between. 7/7 ticks in one capture reproduced it. An
	 * active link never needed the HW monitor anyway -- it already gets
	 * a real, continuously-updated RSSI passively from its own traffic
	 * (mt7925_mlo_link_metric()'s LIVE tier reads wcid->rssi, an ewma of
	 * real RX signal). The HW monitor exists solely to give a dormant
	 * candidate --  one with no organic RX to sample -- a cached estimate
	 * for pair scoring. Rotating across active links was never buying
	 * anything and was actively breaking their RX. Restrict candidates to
	 * links that are valid (resident) but NOT currently traffic-active;
	 * if none exist, every valid link already has a live signal and the
	 * monitor should simply stay disarmed.
	 */
	for (i = 0; i < IEEE80211_MLD_MAX_NUM_LINKS; i++) {
		link_id = (link_id + 1) % IEEE80211_MLD_MAX_NUM_LINKS;
		if ((mvif->valid_links & BIT(link_id)) &&
		    !(vif->active_links & BIT(link_id)))
			break;
	}
	if (!(mvif->valid_links & BIT(link_id)) ||
	    (vif->active_links & BIT(link_id))) {
		/* No dormant candidate exists right now. Disarm using the
		 * link_id that is ACTUALLY currently armed
		 * (mlo_rssi_monitor_link_id, untouched here) -- overwriting it
		 * with this loop's leftover, never-armed candidate first would
		 * make disarm target the wrong link and send no disable
		 * command to the one really armed, leaving it permanently
		 * armed at the firmware level with nothing left to ever
		 * disarm it (every later tick would repeat the same mistake).
		 * Confirmed live: this exact bug kept generating
		 * MLO_STEER_RSSI events, and the WLAN_IDX=4095 bursts that
		 * come with them, forever after the fix that was supposed to
		 * stop arming active links.
		 */
		mt7925_mlo_disarm_rssi_monitor(dev, vif);
		return;
	}

	mt7925_mlo_disarm_rssi_monitor(dev, vif);

	link_conf = mt792x_vif_to_bss_conf(vif, link_id);
	if (!link_conf)
		return;

	/* Persist the rotation position regardless of arm success. Only
	 * gating this on success (as before) meant a SINGLE persistently
	 * failing link's arm attempt would wedge the whole round robin
	 * forever: the next tick's search starts from the same stale
	 * mlo_rssi_monitor_link_id and recomputes the exact same candidate
	 * again, so no other link -- ever -- gets sampled again. Confirmed
	 * live: link0 never appeared in a single MLO_STEER_RSSI dump across
	 * a multi-minute window while link1/link2 kept rotating fine,
	 * leaving every pair candidate that needs link0 permanently stuck
	 * at rssi_score=no-data and therefore never selectable by steering.
	 */
	mvif->mlo_rssi_monitor_link_id = link_id;

	err = mt7925_mcu_set_mlo_rssi_monitor(dev, link_conf, true);
	if (!err) {
		mvif->mlo_rssi_monitor_enabled = true;
	} else if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)) {
		printk(KERN_INFO
		       "MLO_RSSI_MONITOR_ARM_FAIL: link=%u err=%d (rotation still advances)\n",
		       link_id, err);
	}
}

/* Re-evaluate whether the MLO-steering RSSI monitor should be running at
 * all: only while associated, MLD, and mlo_auto_steering is on. Call after
 * any of those three conditions change (association, interface removal,
 * or the mlo_auto_steering debugfs toggle) to disarm promptly -- rotation
 * itself is driven by mt7925_mlo_steer_work()'s own tick.
 */
void mt7925_mlo_update_rssi_monitor(struct mt792x_dev *dev,
				    struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	bool want = mvif->mlo_auto_steering && vif->cfg.assoc &&
		    ieee80211_vif_is_mld(vif);

	if (!want)
		mt7925_mlo_disarm_rssi_monitor(dev, vif);
}
