// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Association/channel/peer execution; direct mac80211, mt76 and MCU dependencies. */
#include <linux/module.h>
#include <linux/etherdevice.h>
#include "../mt7925.h"
#include "../mcu.h"
#include "../mac.h"
#include "../mlo_retouch_primary.h"
#include "pairs.h"
#include "association.h"
#include "radio.h"
#include "../nan.h"

void mt7925_mlo_quiesce_iter(void *data, u8 *mac,
			    struct ieee80211_vif *vif)
{
	mt7925_mlo_completion_assoc(vif, false);
}

void mt7925_mlo_resume_iter(void *data, u8 *mac,
			   struct ieee80211_vif *vif)
{
	mt7925_mlo_completion_assoc(vif, vif->cfg.assoc);
	mt7927_mlo_steering_kick(vif);
}

static bool mt7925_mlo_chanctx_keep_band;
static int mt7927_reconfig_band(struct mt792x_dev *dev,
			      struct ieee80211_vif *vif,
			      struct ieee80211_bss_conf *link_conf,
			      struct mt792x_bss_conf *mconf, u8 band_idx);

/* Caller holds the ordinary device mutex, as before extraction. */
void mt7927_mlo_assoc_link_info_changed(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif,
				       struct ieee80211_bss_conf *info)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);

	if (mvif->mlo_pm_state != MT792x_MLO_CHANGED_PS_PENDING)
		return;
	mt7925_mcu_uni_bss_bcnft(dev, info, true);
	ieee80211_queue_delayed_work(hw, &dev->mlo_pm_work, 5 * HZ);
	mvif->mlo_pm_state = MT792x_MLO_CHANGED_PS;
}

/* TEST-ONLY: mt7925_mcu_bss_mld_tlv() currently sets BSS_MLD's eml_enable
 * true whenever IEEE80211_EML_CAP_EMLSR_SUPP is set in our advertised
 * capability, even though STR is also always advertised (see the comment
 * there). A device can legitimately support both per spec, but eml_enable
 * configures actual firmware/AP operating-mode selection, not just a
 * capability bit -- if firmware or the AP honors it as "operate this
 * association in EMLSR" (single listening radio, MU-RTS-gated per IEEE
 * 802.11be Figure AH-23) that would fight genuine simultaneous dual-radio
 * (STR) operation. This flag forces eml_enable=0 unconditionally so a live
 * run can A/B whether a periodic downlink stall correlates with it.
 */
bool mt7925_test_force_str_no_eml;
module_param_named(test_mt7927_force_str_no_eml, mt7925_test_force_str_no_eml,
		   bool, 0644);
MODULE_PARM_DESC(test_mt7927_force_str_no_eml,
		 "TEST-ONLY: force BSS_MLD eml_enable=0 regardless of advertised EMLSR_SUPP (default N)");

/* TEST-ONLY / EXPERIMENTAL: A/B gate for the survivor-MLD-refresh causal
 * experiment. STA_REC_MLD (mt7925_mcu_sta_mld_tlv()) serializes the peer's
 * current active-link partner table, but a link whose own band_idx does not
 * change across a transition (0x3<->0x5) never gets its STA_REC resent, so
 * its firmware-held partner table can go stale -- proven by source
 * inspection, not yet proven to affect firmware behavior. When ON, the
 * driver additionally reissues the existing, already-production-used
 * mt7925_mcu_sta_authorize() (STATE_ASSOC, newly=0 -- no peer/key/WCID/BA
 * recreation) for each survivor link once mac80211 has published the final
 * new active pair. Default OFF preserves current behaviour exactly.
 */
bool mt7925_test_refresh_survivor_mld;
module_param_named(test_mt7927_refresh_survivor_mld,
		   mt7925_test_refresh_survivor_mld, bool, 0644);
MODULE_PARM_DESC(test_mt7927_refresh_survivor_mld,
		 "EXPERIMENTAL: reissue STA_REC_MLD (STATE_ASSOC,newly=0) for survivor links after an active-pair transition settles (default N)");

/* EXPERIMENTAL: mt7925_mac_set_links() builds the assoc-time JOIN for 2.4G
 * and 5G+6G anchors straight from the STA ASSOC event. That event can run
 * before mt7925_change_vif_links() has registered the partner link's BSS, and
 * mt792x_vif_to_link() then silently hands back the ANCHOR's mconf: the
 * SUB_LINK TLV is built with the anchor's bss_idx (observed: both TLVs
 * bss_idx=0). With this set, the inline JOIN is skipped while the partner BSS
 * is unregistered and the existing change_sta_links JOIN (the path 6G anchors
 * already use) issues it once the BSS exists. Default N keeps today's order.
 */
/* EXPERIMENTAL: the STA ASSOC event requests MLO link activation straight
 * away. mac80211 runs that as the next wiphy work item, so the partner-link
 * BSS/STA/JOIN programming (100+ ms of MCU round trips) holds the wiphy while
 * wpa_supplicant's ASSOC handling (nl80211 queries) waits; the AP's EAPOL M1
 * (+5 ms after AssocResp) then ages past the supplicant's 200 ms replay
 * window and the 4-way never starts. With this set the activation is held
 * until the STA is authorized (4-way finished). Default N.
 */
static bool mt7925_mlo_activate_after_auth;
module_param_named(mlo_activate_after_auth, mt7925_mlo_activate_after_auth,
		   bool, 0644);
MODULE_PARM_DESC(mlo_activate_after_auth,
		 "EXPERIMENTAL: request MT7927 MLO link activation at STA AUTHORIZE instead of ASSOC (default N)");

/* EXPERIMENTAL diagnostic: with mlo_activate_after_auth, do not resume at
 * AUTHORIZE; wait for `echo 1 > mlo_activate_now` (debugfs), so authenticated
 * single-link traffic can be run and traced right up to the JOIN boundary.
 */
static bool mt7925_mlo_activate_hold;
module_param_named(mlo_activate_hold, mt7925_mlo_activate_hold, bool, 0644);
MODULE_PARM_DESC(mlo_activate_hold,
		 "EXPERIMENTAL: keep link activation held after AUTHORIZE until debugfs mlo_activate_now (default N)");

static u8 mt7925_test_mt7927_partner_omac;
module_param_named(test_mt7927_partner_omac, mt7925_test_mt7927_partner_omac,
		   byte, 0644);
MODULE_PARM_DESC(test_mt7927_partner_omac,
		 "EXPERIMENTAL: 1 = give an MT7927 MLD link its own OMAC index only when it collides with a resident link's band slot, 2 = always for links after the first (default 0)");

static bool mt7925_mlo_join_wait_bss = true;
module_param_named(mlo_join_wait_bss, mt7925_mlo_join_wait_bss, bool, 0644);
MODULE_PARM_DESC(mlo_join_wait_bss,
		 "defer the assoc-time MT7927 MLO JOIN until the partner link BSS is registered, so any band can be the association anchor (default Y)");

static u8 mt7925_test_mlo_pair_mask;
module_param_named(test_mt7927_mlo_pair_mask, mt7925_test_mlo_pair_mask,
		   byte, 0644);
MODULE_PARM_DESC(test_mt7927_mlo_pair_mask,
		 "TEST-ONLY MT7927 pre-activation MLO pair mask (0, 0x3, 0x5, or 0x6)");


/* TEST-ONLY: probe whether MT7927 MLO's silent link1/5GHz TX symptom is
 * caused by wmm_idx being pinned to 0 for every link of an MLD. wmm_idx
 * feeds MT_TXD0_Q_IDX (mt7925/mac.c, mt7925_mac_write_txwi():
 * q_idx = wmm_idx * MT76_CONNAC_MAX_WMM_SETS + ac) -- the hardware LMAC
 * TX queue index, separate from but alongside band_idx (which feeds the
 * MT_TXD1_TGID field selecting the DBDC PHY). With wmm_idx=0 on every
 * link, a band_idx=0 link and a band_idx=1 link on the same AC compute an
 * IDENTICAL q_idx, so both bind to the same hardware LMAC queue context.
 * Default OFF; MT7927 MLO station links only; does not touch the band_idx
 * allocator itself.
 */
static bool mt7925_test_mt7927_wmm_band_couple;
module_param_named(test_mt7927_wmm_band_couple,
		   mt7925_test_mt7927_wmm_band_couple, bool, 0644);
MODULE_PARM_DESC(test_mt7927_wmm_band_couple,
		 "TEST-ONLY: set MT7927 MLO wmm_idx = band_idx instead of pinning it to 0");

/* Dedicated fixed-priority resolver for the only collision case
 * mt7927_band_idx() can ever produce: 5GHz and 6GHz both naturally map to
 * band_idx=1. 2.4GHz keeps its own distinct natural value (0) and is
 * never part of this decision -- forcing a 2.4-vs-other collision was
 * tested live and broke 3-link MLD negotiation outright, unlike a 5/6GHz
 * collision which negotiates fine and only affects which link's OTA
 * emission works (see OTA_band_idx_findings.md, checked into PR #99).
 *
 * The previous resolution here was reactive: whichever link's BSS got
 * programmed to firmware SECOND lost the collision and got flipped to 0,
 * regardless of which band it was. That depended on mac80211's per-
 * association link-add ordering (in turn driven by which BSSID the STA
 * happened to anchor its initial association on), which is not stable
 * across associations -- every trace this session happened to put
 * 5GHz second, so 5GHz always lost, but nothing in the code guaranteed
 * that.
 *
 * This resolves it purely by band identity instead: 6GHz always keeps
 * the natural slot (1), 5GHz always takes the alternate slot (0),
 * decided the moment EITHER link's BSS is programmed, by checking
 * vif->valid_links -- the mac80211-core field, already populated with
 * the full negotiated link set before the driver's first per-link BSS
 * add runs, unlike mvif->valid_links (the driver's own bookkeeping,
 * which only catches up after each add loop finishes). That makes the
 * decision available on the FIRST link's add call, not just the second,
 * with no dependency on which order the two links actually get added in.
 * Once mac80211 has selected a two-link operational pair, use that pair:
 * a negotiated but dormant 6GHz member must not put an active 5GHz member
 * onto the 2.4GHz member's slot during chanctx assignment.
 */
static u8 mt7927_mlo_5g6g_band_idx(struct ieee80211_vif *vif,
				   unsigned int link_id,
				   enum nl80211_band band)
{
	unsigned long members = vif->valid_links;
	unsigned long other_links;
	unsigned int other_id;

	if (hweight16(vif->active_links) == 2 &&
	    (vif->active_links & BIT(link_id)))
		members = vif->active_links;
	other_links = members & ~BIT(link_id);

	if (band != NL80211_BAND_5GHZ && band != NL80211_BAND_6GHZ)
		return mt7927_band_idx(band);

	for_each_set_bit(other_id, &other_links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_bss_conf *other_conf =
			mt792x_vif_to_bss_conf(vif, other_id);
		enum nl80211_band other_band;

		if (!other_conf || !other_conf->chanreq.oper.chan)
			continue;

		other_band = other_conf->chanreq.oper.chan->band;
		if (other_band != NL80211_BAND_5GHZ &&
		    other_band != NL80211_BAND_6GHZ)
			continue;

		return band == NL80211_BAND_6GHZ ? 1 : 0;
	}

	return mt7927_band_idx(band);
}

static void mt7927_mld_assign_band_idx(struct mt792x_dev *dev,
				       struct ieee80211_bss_conf *link_conf,
				       struct mt792x_bss_conf *mconf,
				       struct ieee80211_channel *chan)
{
	struct ieee80211_vif *vif = link_conf->vif;
	u8 band_idx;

	if (!is_mt7927(&dev->mt76) ||
	    !ieee80211_vif_is_mld(vif) ||
	    vif->type != NL80211_IFTYPE_STATION) {
		mconf->mt76.band_idx = mt7927_band_idx(chan->band);
		return;
	}

	band_idx = mt7927_mlo_5g6g_band_idx(vif, link_conf->link_id,
					    chan->band);
	if (band_idx != mconf->mt76.band_idx)
		dev_info(dev->mt76.dev,
			 "MT7927 MLO BSS band context (fixed-priority): link_id=%u bss_idx=%u band=%d assigned=%u\n",
			 link_conf->link_id, mconf->mt76.idx, chan->band,
			 band_idx);
	mconf->mt76.band_idx = band_idx;
}

static u8 mt7927_mlo_wmm_idx(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			     u8 band_idx, u8 default_wmm_idx)
{
	if (!READ_ONCE(mt7925_test_mt7927_wmm_band_couple) ||
	    !is_mt7927(&dev->mt76) || !ieee80211_vif_is_mld(vif) ||
	    vif->type != NL80211_IFTYPE_STATION)
		return default_wmm_idx;

	return band_idx;
}

void mt7927_mlo_assoc_prepare_bss(struct mt792x_dev *dev,
				   struct ieee80211_bss_conf *link_conf,
				   struct mt792x_bss_conf *mconf)
{
	struct ieee80211_vif *vif = link_conf->vif;
	struct mt792x_vif *mvif = mconf->vif;

	/* Affiliated links belong to one MLD hardware interface. Keep their
	 * BSS indices distinct, but program the shared OMAC identity expected
	 * by the firmware datapath.
	 */
	mconf->mt76.omac_idx = ieee80211_vif_is_mld(vif) ?
			       0 : mconf->mt76.idx;
	mconf->mt76.band_idx = 0xff;

	if (is_mt7927(&dev->mt76)) {
		struct ieee80211_channel *chan = link_conf->chanreq.oper.chan;
		const char *src = "chanreq";

		/* A link's own channel is not assigned yet while its BSS is
		 * created (the chanctx arrives later). Never fall back to the
		 * phy's previous channel for an MLD link: that is whatever the
		 * last scan/association left behind (observed: a 5 GHz link
		 * given a 6 GHz band context). The scan entry cfg80211 holds
		 * for the link's BSSID is the authoritative channel.
		 */
		if (!chan && ieee80211_vif_is_mld(vif) &&
		    vif->type == NL80211_IFTYPE_STATION) {
			struct cfg80211_bss *cbss;

			cbss = cfg80211_get_bss(mvif->phy->mt76->hw->wiphy, NULL,
						link_conf->bssid, NULL, 0,
						IEEE80211_BSS_TYPE_ANY,
						IEEE80211_PRIVACY_ANY);
			if (cbss) {
				chan = cbss->channel;
				src = "cfg80211_bss";
				cfg80211_put_bss(mvif->phy->mt76->hw->wiphy,
						 cbss);
			}
		}

		if (!chan && ieee80211_vif_is_mld(vif) &&
		    vif->type == NL80211_IFTYPE_STATION) {
			/* Neither the link's chanreq nor a scan entry for its
			 * BSSID exists yet (mac80211 creates the link before it
			 * knows the BSSID or assigns the chanctx). Do not guess
			 * from the phy: install a placeholder context (the
			 * shared 5/6 GHz slot) that is replaced with the real
			 * value in mt7925_assign_vif_chanctx(), before the first
			 * frame is sent on the link.
			 */
			mconf->mt76.band_idx = 1;
			if (READ_ONCE(mt76_mlo_diag_trace) ||
			    READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_BSS_CHAN: link_id=%u src=none provisional_band_idx=1 (resolved at chanctx assign) phy_chandef_chan=%u phy_chandef_band=%d IGNORED\n",
				       link_conf->link_id,
				       mvif->phy->mt76->chandef.chan->hw_value,
				       mvif->phy->mt76->chandef.chan->band);
			goto bss_chan_done;
		}

		if (!chan) {
			chan = mvif->phy->mt76->chandef.chan;
			src = "phy_chandef(non-MLD)";
		}

		if (READ_ONCE(mt76_mlo_diag_trace) ||
		    READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			       "MLO_BSS_CHAN: link_id=%u src=%s chan=%u band=%d phy_chandef_chan=%u phy_chandef_band=%d bssid=%pM\n",
			       link_conf->link_id, src, chan->hw_value,
			       chan->band,
			       mvif->phy->mt76->chandef.chan->hw_value,
			       mvif->phy->mt76->chandef.chan->band,
			       link_conf->bssid);

		mt7927_mld_assign_band_idx(dev, link_conf, mconf, chan);
bss_chan_done:
		;
	}

	mconf->mt76.wmm_idx = mt7927_mlo_wmm_idx(dev, vif, mconf->mt76.band_idx,
						 ieee80211_vif_is_mld(vif) ? 0 :
						 mconf->mt76.idx % MT76_CONNAC_MAX_WMM_SETS);
	mconf->mt76.link_idx = hweight16(mvif->valid_links);

	/* EXPERIMENT (default N): every MLD link shares OMAC 0 and DEV_INFO
	 * programs the OMAC/band entry from the link being added. Measured on a
	 * live L1-first association: the instant the 6 GHz link's DEV (omac 0,
	 * band 1) was added next to the live 5 GHz link (omac 0, band 1), the
	 * 5 GHz link stopped emitting (txwi -> no RTS on air) although every
	 * command returned 0. With the added link on its own OMAC the live link
	 * kept carrying traffic through the same sequence. Only when the added
	 * link would take a band slot another resident link still holds does it
	 * get its own OMAC index; non-colliding pairs keep the shared OMAC 0.
	 */
	if (READ_ONCE(mt7925_test_mt7927_partner_omac) && is_mt7927(&dev->mt76) &&
	    ieee80211_vif_is_mld(vif) && vif->type == NL80211_IFTYPE_STATION &&
	    mconf->mt76.link_idx && mconf->mt76.link_idx < 4) {
		unsigned int other;
		bool collide = false;

		for_each_set_bit(other, (unsigned long *)&mvif->valid_links,
				 IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_bss_conf *oc =
				rcu_dereference_protected(mvif->link_conf[other],
					lockdep_is_held(&dev->mt76.mutex));

			if (oc && oc != mconf && oc->mt76.band_idx == mconf->mt76.band_idx)
				collide = true;
		}
		if (collide || READ_ONCE(mt7925_test_mt7927_partner_omac) == 2) {
			mconf->mt76.omac_idx = mconf->mt76.link_idx;
			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_PARTNER_OMAC: link_id=%u band_idx=%u collides_or_forced; omac_idx=%u\n",
				       link_conf->link_id, mconf->mt76.band_idx,
				       mconf->mt76.omac_idx);
		}
	}

}

int
mt7927_mlo_assoc_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	int ret;

	mt7925_mlo_completion_init(vif);
	mt792x_mutex_acquire(dev);
	mvif->phy = mt792x_hw_phy(hw);
	if (is_mt7927(&dev->mt76))
		mt7927_mlo_pairs_init(vif);
	mvif->mlo_pm_state = MT792x_MLO_LINK_DISASSOC;
	ret = mt7925_add_interface_resources(hw, vif);
	if (!ret) {
		memset(mvif->mlo_link_rssi, MT792X_MLO_RSSI_UNKNOWN,
		       sizeof(mvif->mlo_link_rssi));
		mt7927_mlo_steering_init(vif);
	}
	mt792x_mutex_release(dev);
	return ret;
}

void
mt7927_mlo_assoc_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);

	/* Close the producer gate before joining an ordinary worker. It never
	 * acquires/waits on wiphy, so joining it under wiphy cannot invert locks.
	 */
	mvif->mlo_auto_steering = false;
	mt7925_mlo_join_defer_drop(mvif, "remove_interface");
	mt7925_mlo_activate_defer_drop(mvif, "remove_interface");
	mt7925_mlo_completion_stop(vif);
	mt7925_mlo_update_rssi_monitor(dev, vif);

	mt792x_mutex_acquire(dev);

	if (dev->nan_vif == vif)
		dev->nan_vif = NULL;

	mt792x_mac_link_bss_remove(dev, mt792x_link_conf_to_mconf(&vif->bss_conf),
				   &mvif->sta.deflink);

	mt792x_mutex_release(dev);
}


int mt7927_mlo_assoc_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			  struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			  struct ieee80211_key_conf *key)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_sta *msta = sta ? (struct mt792x_sta *)sta->drv_priv :
				  &mvif->sta;
	struct mt792x_link_sta *mlink;
	int err;

	mt7925_mlo_completion_key_changed(vif);
	mt792x_mutex_acquire(dev);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_KEY_INSTALL: cmd=%d cipher=0x%x key_link_id=%d\n",
		      cmd, key->cipher, key->link_id);

	if (cmd == SET_KEY)
		mt76_mlo_bssinfo_deferred_send();

	if (ieee80211_vif_is_mld(vif)) {
		unsigned int link_id;
		unsigned long add;

		add = key->link_id != -1 ? BIT(key->link_id) : msta->valid_links;

		for_each_set_bit(link_id, &add, IEEE80211_MLD_MAX_NUM_LINKS) {
			mlink = mt792x_sta_to_link(msta, link_id);
			err = mt7925_set_link_key(hw, cmd, vif, sta, key, link_id,
						  mlink);
			if (err < 0)
				break;

			/* SET_KEY trigger for the postkey standalone SUB_LINK
			 * ROC experiment: arm stage 1 once mac80211's own
			 * ieee80211_key_switch_links() has actually enabled
			 * a pending substitute's group key on this link. The
			 * peer sta==NULL for group-key installs, so msta
			 * above is the BMC pseudo-STA, not the peer -- the
			 * real peer is resolved fresh here, under RCU, only
			 * long enough to queue the work item. No ROC, wait,
			 * or mutex recursion happens in this function.
			 */
			if (cmd == SET_KEY &&
			    !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE) &&
			    READ_ONCE(mt7925_test_mt7927_sublink_postkey) &&
			    READ_ONCE(mt7925_test_mt7927_sublink_roc) &&
			    is_mt7927(&dev->mt76) &&
			    vif->type == NL80211_IFTYPE_STATION) {
				struct ieee80211_sta *psta;

				rcu_read_lock();
				psta = ieee80211_find_sta(vif, vif->cfg.ap_addr);
				if (psta) {
					struct mt792x_sta *pmsta =
						(struct mt792x_sta *)psta->drv_priv;

					if (pmsta->postkey_pending &&
					    !pmsta->postkey_roc_attempted &&
					    pmsta->postkey_generation ==
						    mvif->mt7927_mlo.transition_generation &&
					    link_id == pmsta->postkey_sublink_id) {
						if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
							printk(KERN_INFO
							      "POSTKEY_SETKEY_ARM: link=%u gen=%u cipher=0x%x\n",
							      link_id,
							      pmsta->postkey_generation,
							      key->cipher);
						wiphy_work_queue(hw->wiphy,
								 &pmsta->postkey_stage1);
					}
				}
				rcu_read_unlock();
			}
		}
	}

	mt792x_mutex_release(dev);
	mt7925_mlo_completion_note_error(vif, err);

	return err;
}

void mt7927_mlo_assoc_unassign_vif_chanctx(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif,
					struct ieee80211_bss_conf *link_conf,
					struct ieee80211_chanctx_conf *ctx)
{
	struct mt792x_chanctx *mctx = (struct mt792x_chanctx *)ctx->drv_priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_bss_conf *mconf;

	mutex_lock(&dev->mt76.mutex);

	if (ieee80211_vif_is_mld(vif)) {
		mconf = mt792x_vif_to_link(mvif, link_conf->link_id);

		if (vif->type == NL80211_IFTYPE_STATION &&
		    mconf == &mvif->bss_conf)
			mt7925_mcu_add_bss_info(&dev->phy, NULL, link_conf,
						NULL, false);
	} else {
		mconf = &mvif->bss_conf;
	}

	mctx->bss_conf = NULL;
	mconf->mt76.ctx = NULL;
	mutex_unlock(&dev->mt76.mutex);

	if (link_conf->csa_active) {
		timer_delete_sync(&mvif->csa_timer);
		cancel_work_sync(&mvif->csa_work);
	}
}

int mt7927_mlo_assoc_assign_vif_chanctx(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *link_conf,
				     struct ieee80211_chanctx_conf *ctx)
{
	struct mt792x_chanctx *mctx = (struct mt792x_chanctx *)ctx->drv_priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct ieee80211_bss_conf *pri_link_conf;
	struct mt792x_bss_conf *mconf;
	u8 band_idx;

	mutex_lock(&dev->mt76.mutex);

	if (ieee80211_vif_is_mld(vif)) {
		mconf = mt792x_vif_to_link(mvif, link_conf->link_id);
		pri_link_conf = mt792x_vif_to_bss_conf(vif, mvif->deflink_id);

		/* The link's real channel is known only now. Replace the
		 * placeholder band context installed at BSS creation with the
		 * one derived from this channel, and rebind the firmware DEV
		 * to it, before anything is transmitted on the link.
		 */
		if (is_mt7927(&dev->mt76) && vif->type == NL80211_IFTYPE_STATION &&
		    READ_ONCE(mt7925_mlo_chanctx_keep_band) && mconf != &mvif->bss_conf) {
			if (READ_ONCE(mt76_mlo_diag_trace) ||
			    READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_BSS_CHAN: link_id=%u src=chanctx chan=%u band=%d keep band_idx %u (pair-aware assignment retained)\n",
				       link_conf->link_id, ctx->def.chan->hw_value,
				       ctx->def.chan->band, mconf->mt76.band_idx);
		} else if (is_mt7927(&dev->mt76) && vif->type == NL80211_IFTYPE_STATION) {
			u8 old_idx = mconf->mt76.band_idx;

			mt7927_mld_assign_band_idx(dev, link_conf, mconf,
						   ctx->def.chan);
			if (READ_ONCE(mt76_mlo_diag_trace) ||
			    READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_BSS_CHAN: link_id=%u src=chanctx chan=%u band=%d band_idx %u->%u\n",
				       link_conf->link_id, ctx->def.chan->hw_value,
				       ctx->def.chan->band, old_idx,
				       mconf->mt76.band_idx);
			if (mconf == &mvif->bss_conf &&
			    mconf->mt76.band_idx != old_idx) {
				u8 new_idx = mconf->mt76.band_idx;

				mconf->mt76.band_idx = old_idx;
				mt7927_reconfig_band(dev, vif, link_conf, mconf,
						     new_idx);
				mconf->mt76.band_idx = new_idx;
				mvif->sta.deflink.wcid.phy_idx = new_idx;
			}
		}

		if (vif->type == NL80211_IFTYPE_STATION &&
		    mconf == &mvif->bss_conf)
			mt7925_mcu_add_bss_info(&dev->phy, NULL, pri_link_conf,
						NULL, true);
	} else {
		mconf = &mvif->bss_conf;

		if (is_mt7927(&dev->mt76)) {
			band_idx = mt7927_band_idx(ctx->def.chan->band);

			mt7927_reconfig_band(dev, vif, link_conf, mconf, band_idx);
			mconf->mt76.band_idx = band_idx;
			mvif->sta.deflink.wcid.phy_idx = band_idx;
		}
	}

	/* A monitor vif gets a fresh chanctx on every channel/band change;
	 * re-arm the sniffer here so it actually retunes instead of staying
	 * stuck on the band where monitor was first enabled. Do this before
	 * mconf->mt76.ctx is pointed at the new ctx below: the helper needs
	 * to see the still-old ctx (or NULL, on first assignment) to detect
	 * a cross-band move correctly; it sets mconf->mt76.ctx itself once
	 * the old-band teardown is done.
	 */
	if (vif->type == NL80211_IFTYPE_MONITOR) {
		mt7925_monitor_arm_sniffer(mvif->phy, vif, ctx);
	} else {
		mconf->mt76.ctx = ctx;
	}
	mctx->bss_conf = mconf;
	mt76_phy_chandef_set(mvif->phy->mt76, &ctx->def);
	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

static int
mt7927_reconfig_band(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		     struct ieee80211_bss_conf *link_conf,
		     struct mt792x_bss_conf *mconf,
		     u8 band_idx)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_link_sta *mlink = &mvif->sta.deflink;
	int ret;

	ret = mt76_connac_mcu_uni_add_dev(&dev->mphy, link_conf,
					  &mconf->mt76, &mlink->wcid,
					  false);
	if (ret)
		return ret;

	mconf->mt76.band_idx = band_idx;

	return mt76_connac_mcu_uni_add_dev(&dev->mphy, link_conf,
					   &mconf->mt76, &mlink->wcid,
					   true);
}

/* EXPERIMENTAL (default N, A/B): mt7925_assign_vif_chanctx() recomputes a link's band context with the valid-link-set rule
 * (mt7927_mlo_5g6g_band_idx(): a 5 GHz link gets slot 0 whenever ANY other valid link is 5/6 GHz). For a link that is
 * added later it runs AFTER the pair-aware assignment of the transition (mt7927_mlo_pair_band_idx()), reverting a 5 GHz
 * incoming link of a 2.4+5 GHz pair to slot 0, the same slot as the 2.4 GHz survivor; its STA_REC is then created on that
 * slot and the firmware leaves it unattached to the MLD (measured: peer MLD address = AP link address, no related WCID).
 * With this set only the deflink (association-time placeholder) is recomputed here.
 */
module_param_named(mlo_chanctx_keep_band, mt7925_mlo_chanctx_keep_band, bool, 0644);
MODULE_PARM_DESC(mlo_chanctx_keep_band,
		 "EXPERIMENTAL: do not recompute a non-deflink MT7927 MLD link's band context at chanctx assign (default N)");

int
mt7927_mlo_assoc_change_sta_links(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			struct ieee80211_sta *sta, u16 old_links, u16 new_links)
{
	unsigned long add = new_links & ~old_links;
	unsigned long rem = old_links & ~new_links;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	int err = 0;

	dev_info(dev->mt76.dev,
		 "EXPERIMENTAL MLO TEST: change_sta_links old=0x%x new=0x%x add=0x%lx rem=0x%lx\n",
		 old_links, new_links, add, rem);

	if (old_links == new_links)
		return 0;

	mt7925_mlo_completion_changed(vif);
	if (old_links && new_links &&
	    is_mt7927(&dev->mt76) &&
	    ieee80211_vif_is_mld(vif) &&
	    vif->type == NL80211_IFTYPE_STATION) {
		err = mt7927_mlo_change_sta_links(hw, vif, sta, old_links, new_links);
		mt7925_mlo_completion_note_error(vif, err);
		return err;
	}

	mt7925_mlo_clear_tid_link((struct mt792x_sta *)sta->drv_priv, -1);

	mt792x_mutex_acquire(dev);

	err = mt7925_mac_sta_remove_links(dev, vif, sta, rem);
	if (err < 0)
		goto out;

	err = mt7925_mac_sta_add_links(dev, vif, sta, add);
	if (err < 0)
		goto out;

	/* Real bug found live: mt7925_mac_sta_remove_links() resets
	 * msta->deflink_id to IEEE80211_LINK_UNSPECIFIED when the CURRENT
	 * deflink itself is removed from active_links (existing code, a few
	 * lines up in this file). mt7925_mac_sta_add_links() then picks
	 * whichever link in `add` is processed first as the NEW deflink,
	 * but only creates it at STA_REC state=NONE -- the same low state
	 * a truly fresh deflink starts at before mac80211's real
	 * AUTH->ASSOC event upgrades it to PORT_SECURE/ASSOC via
	 * mt7925_mac_sta_event(). That event fires exactly once per STA,
	 * at the original handshake; it never fires again for a deflink
	 * identity that migrates to a different link_id later during a
	 * pair transition. Live testing proved the effect: switching from
	 * a pair whose deflink got deactivated (e.g. 0x6 -> 0x3, deflink
	 * on link2) left BOTH newly active links -- the reassigned deflink
	 * and the genuine secondary -- stuck at zero real TX airtime, even
	 * though the secondary path above already tries to admit
	 * non-deflink adds. Admit every link now in `new_links` here,
	 * unconditionally: idempotent for links already properly admitted,
	 * and it is the only remaining point that reliably runs on every
	 * active-link transition regardless of which link becomes deflink.
	 */
	if (ieee80211_vif_is_mld(vif)) {
		unsigned long admit_links = new_links;
		unsigned int link_id;

		for_each_set_bit(link_id, &admit_links,
				 IEEE80211_MLD_MAX_NUM_LINKS) {
			struct ieee80211_link_sta *link_sta =
				mt792x_sta_to_link_sta(vif, sta, link_id);

			if (link_sta) {
				err = mt7925_mac_link_sta_assoc(dev, vif, link_sta);
				if (err)
					goto out;
			}
		}
	}

out:
	mt792x_mutex_release(dev);

	return err;
}

/* TEST-ONLY partner-info ordering A/B (default off): the stock order sends
 * the primary STA_REC (with STA_REC_MLD already listing the incoming link's
 * WCID/BSS) BEFORE the incoming link's own STA_REC is created in firmware.
 * With this set, the incoming STA_REC is created first and the primary MLD
 * update that references it follows.
 */
static bool mt7925_test_partner_after_create;
module_param_named(test_mt7927_partner_after_create,
		   mt7925_test_partner_after_create, bool, 0644);
MODULE_PARM_DESC(test_mt7927_partner_after_create,
		 "TEST-ONLY MT7927 create the incoming link STA_REC before the primary MLD update that references it (default off)");

/* TEST-ONLY (requires test_mt7927_partner_after_create): withhold the
 * survivor primary's STA_REC ASSOC authorize during its destructive
 * recreate when a partner link is being added in the same transition, so the
 * only ASSOC the primary ever receives is the one carrying the complete
 * two-link STA_REC_MLD, sent after the partner's own STA_REC exists.
 */
static bool mt7925_test_defer_primary_authorize;
module_param_named(test_mt7927_defer_primary_authorize,
		   mt7925_test_defer_primary_authorize, bool, 0644);
MODULE_PARM_DESC(test_mt7927_defer_primary_authorize,
		 "TEST-ONLY MT7927 authorize the survivor primary only after the partner STA_REC exists and the full two-link MLD is programmed (needs test_mt7927_partner_after_create; default off)");

bool mt7925_defer_primary_authorize_active(void)
{
	return READ_ONCE(mt7925_test_defer_primary_authorize) &&
	       READ_ONCE(mt7925_test_partner_after_create);
}

static int mt7925_partner_retouch(struct mt792x_dev *dev,
				  struct ieee80211_vif *vif,
				  struct mt792x_link_sta *mlink,
				  struct mt792x_link_sta *pri_mlink,
				  bool incoming_created)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt76_wcid *pri_wcid = &pri_mlink->wcid;
	struct mt792x_bss_conf *pri_conf, *sec_conf;
	bool swap = READ_ONCE(mt7925_test_partner_after_create);

	pri_conf = mt792x_vif_to_link(mvif, pri_wcid->link_id);
	sec_conf = mt792x_vif_to_link(mvif, mlink->wcid.link_id);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_PARTNER_ORDER: swap=%u incoming_sta_rec_created=%u pri(link=%u wcid=%u bss=%d omac=%d band=%d valid=%u) sec(link=%u wcid=%u bss=%d omac=%d band=%d valid=%u) vif=%pM\n",
		       swap, incoming_created,
		       pri_wcid->link_id, pri_wcid->idx,
		       pri_conf ? pri_conf->mt76.idx : -1,
		       pri_conf ? pri_conf->mt76.omac_idx : -1,
		       pri_conf ? pri_conf->mt76.band_idx : -1,
		       pri_wcid->link_valid,
		       mlink->wcid.link_id, mlink->wcid.idx,
		       sec_conf ? sec_conf->mt76.idx : -1,
		       sec_conf ? sec_conf->mt76.omac_idx : -1,
		       sec_conf ? sec_conf->mt76.band_idx : -1,
		       mlink->wcid.link_valid, vif->addr);

	if (swap && (!pri_conf || !sec_conf || !pri_wcid->link_valid ||
		     !mlink->wcid.link_valid || !incoming_created)) {
		dev_err(dev->mt76.dev,
			"MLO_PARTNER_ORDER: refusing MLD update, referenced entry missing\n");
		return -EINVAL;
	}

	{
		int ret = mt7925_mcu_sta_update(dev, mlink->pri_link, vif,
							pri_mlink, true,
							MT76_STA_INFO_STATE_ASSOC,
							mlink);

		if (!ret && pri_mlink->sta &&
		    (pri_mlink->sta->mlo_defer_auth_links & BIT(pri_wcid->link_id))) {
			pri_mlink->sta->mlo_defer_auth_links &=
				~BIT(pri_wcid->link_id);
			if (READ_ONCE(mt76_mlo_diag_trace) ||
			    READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_PRIMARY_AUTH_DEFERRED_DONE: pri_link=%u pri_wcid=%u partner_link=%u partner_wcid=%u via=partner_retouch ret=0\n",
				       pri_wcid->link_id, pri_wcid->idx,
				       mlink->wcid.link_id, mlink->wcid.idx);
		}
		return ret;
	}
}

/* A pair transition can retire the host deflink; with this set the primary retouch that lists an incoming partner
 * in STA_REC_MLD goes to a live member instead of the retired deflink (default off).
 */
static bool mt7925_mlo_live_retouch;
module_param_named(mlo_live_retouch, mt7925_mlo_live_retouch, bool, 0644);
MODULE_PARM_DESC(mlo_live_retouch,
		 "EXPERIMENTAL: MT7927 address the primary STA_REC retouch to a live MLD member, not a retired deflink (default N)");

static int mt7925_mac_link_sta_add(struct mt76_dev *mdev,
				   struct ieee80211_vif *vif,
				   struct ieee80211_link_sta *link_sta,
				   struct mt792x_link_sta *mlink)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_bss_conf *link_conf;
	struct mt792x_bss_conf *mconf;
	u8 link_id = link_sta->link_id;
	bool wcid_published = false;
	struct mt792x_sta *msta;
	struct mt76_wcid *wcid;
	bool pm_woken = false;
	int ret, idx;

	msta = (struct mt792x_sta *)link_sta->sta->drv_priv;

	if (WARN_ON_ONCE(!mlink))
		return -EINVAL;

	dev_info(dev->mt76.dev,
		 "EXPERIMENTAL MLO TEST: link_sta_add link_id=%u is_pri=%d valid_links=0x%x\n",
		 link_id, link_sta == mlink->pri_link, link_sta->sta->valid_links);

	idx = mt76_wcid_alloc(dev->mt76.wcid_mask, MT792x_WTBL_STA - 1);
	if (idx < 0) {
		dev_info(dev->mt76.dev,
			 "EXPERIMENTAL MLO TEST: link_id=%u wcid_alloc FAILED (idx=%d)\n",
			 link_id, idx);
		return -ENOSPC;
	}

	mconf = mt792x_vif_to_link(mvif, link_id);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_WCID_LIFE: stage=add_alloc link=%u mlink=%px wcid=%px pre_idx=%u mconf=%px bss=%u omac=%u band=%u hwkey=%d/%d cipher=%u tx_info=0x%x valid=%u\n",
		       link_id, mlink, &mlink->wcid, mlink->wcid.idx, mconf,
		       mconf ? mconf->mt76.idx : 0xff,
		       mconf ? mconf->mt76.omac_idx : 0xff,
		       mconf ? mconf->mt76.band_idx : 0xff,
		       mlink->wcid.hw_key_idx, mlink->wcid.hw_key_idx2,
		       mlink->wcid.cipher, mlink->wcid.tx_info,
		       mlink->wcid.link_valid);
	mt76_wcid_init(&mlink->wcid, 0);
	/* RX band cross-check (mt7925_mac_fill_rx) trusts wcid.phy_idx as the
	 * link's band slot; a peer created with the default 0 on band 1 makes
	 * two siblings claim band 0.
	 */
	if (mconf && is_mt7927(&dev->mt76) && ieee80211_vif_is_mld(vif) &&
	    READ_ONCE(mt7925_mlo_sync_peer_phy))
		mlink->wcid.phy_idx = mconf->mt76.band_idx;
	mlink->wcid.sta = 1;
	mlink->wcid.idx = idx;
	mlink->wcid.tx_info |= MT_WCID_TX_INFO_SET;
	mlink->last_txs = jiffies;
	mlink->wcid.link_id = link_sta->link_id;
	mlink->wcid.link_valid = !!link_sta->sta->valid_links;
	mlink->sta = msta;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_WCID_LIFE: stage=after_wcid_init link=%u mlink=%px wcid=%px idx=%u mconf=%px bss=%u omac=%u band=%u phy=%u hwkey=%d/%d cipher=%u tx_info=0x%x valid=%u sta_valid=0x%x deflink_id=%u\n",
		       link_id, mlink, &mlink->wcid, mlink->wcid.idx, mconf,
		       mconf ? mconf->mt76.idx : 0xff,
		       mconf ? mconf->mt76.omac_idx : 0xff,
		       mconf ? mconf->mt76.band_idx : 0xff,
		       mlink->wcid.phy_idx, mlink->wcid.hw_key_idx,
		       mlink->wcid.hw_key_idx2, mlink->wcid.cipher,
		       mlink->wcid.tx_info, mlink->wcid.link_valid,
		       msta->valid_links, msta->deflink_id);

	if (link_sta->sta->tdls)
		set_bit(MT_WCID_FLAG_TDLS_PEER, &mlink->wcid.flags);

	wcid = &mlink->wcid;
	ewma_signal_init(&wcid->rssi);
	rcu_assign_pointer(dev->mt76.wcid[wcid->idx], wcid);
	wcid_published = true;
	ewma_avg_signal_init(&mlink->avg_ack_signal);
	memset(mlink->airtime_ac, 0,
	       sizeof(msta->deflink.airtime_ac));

	ret = mt76_connac_pm_wake(&dev->mphy, &dev->pm);
	if (ret)
		goto out_wcid;
	pm_woken = true;

	mt7925_mac_wtbl_update(dev, idx,
			       MT_WTBL_UPDATE_ADM_COUNT_CLEAR);

	link_conf = mt792x_vif_to_bss_conf(vif, link_id);

/* compat: the cfg80211 NAN API arrived in kernel 7.2 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
	/* NAN_DATA (NDI) peers skip association - fill link_sta caps
	 * from sband and push BSS_INFO with correct phymode + RLM.
	 */
	if (vif->type == NL80211_IFTYPE_NAN_DATA) {
		struct ieee80211_chanctx_conf *nan_ctx;

		nan_ctx = mt7925_nan_seed_link_sta(dev, link_sta);
		mconf->mt76.ctx = nan_ctx;

		ret = mt7925_mcu_add_bss_info(&dev->phy, nan_ctx,
					      link_conf, link_sta, true);
		if (ret)
			goto out_pm;
	}
#endif

	/* should update bss info before STA add */
	if (vif->type == NL80211_IFTYPE_STATION && !link_sta->sta->tdls) {
		struct mt792x_link_sta *mlink_bc;

		mlink_bc = mt792x_sta_to_link(&mvif->sta, mconf->link_id);

		if (ieee80211_vif_is_mld(vif)) {
			ret = mt7925_mcu_add_bss_info_sta(&dev->phy, mconf->mt76.ctx,
							  link_conf, link_sta,
							  mlink_bc->wcid.idx, mlink->wcid.idx,
							  link_sta != mlink->pri_link);
			if (ret)
				goto out_pm;
		} else {
			ret = mt7925_mcu_add_bss_info_sta(&dev->phy, mconf->mt76.ctx,
							  link_conf, link_sta,
							  mlink_bc->wcid.idx, mlink->wcid.idx,
							  false);
			if (ret)
				goto out_pm;
		}
	}

	if (ieee80211_vif_is_mld(vif) &&
	    link_sta == mlink->pri_link) {
		ret = mt7925_mcu_sta_update(dev, link_sta, vif,
					    mlink, true,
					    MT76_STA_INFO_STATE_NONE, NULL);
		if (ret)
			goto out_pm;
	} else if (ieee80211_vif_is_mld(vif) &&
		   link_sta != mlink->pri_link) {
		struct mt792x_link_sta *pri_mlink;
		struct mt76_wcid *pri_wcid;

		/* alternative lookup via def_wcid */
		pri_wcid = mlink->wcid.def_wcid;

		pri_mlink = pri_wcid ?
			    container_of(pri_wcid, struct mt792x_link_sta, wcid) :
			    NULL;

		if (WARN_ON_ONCE(!pri_mlink)) {
			ret = -EINVAL;
			goto out_pm;
		}

		/* def_wcid is the association's host deflink, which a pair
		 * transition may have retired. The retouch carries the
		 * two-link STA_REC_MLD and must reach a live member, or the
		 * survivor keeps pointing at the retired WCID in firmware and
		 * the incoming WCID stays unattached.
		 */
		if (is_mt7927(&dev->mt76) && READ_ONCE(mt7925_mlo_live_retouch)) {
			struct mt792x_sta *retouch_msta = mlink->sta;
			unsigned long live = 0;
			unsigned int id;
			int rid;

			for (id = 0; id < IEEE80211_MLD_MAX_NUM_LINKS; id++) {
				struct mt792x_link_sta *cand =
					mt792x_sta_to_link(retouch_msta, id);

				if (cand && cand->wcid.link_valid)
					live |= BIT(id);
			}
			rid = mt7925_mlo_retouch_primary_link(retouch_msta->deflink_id,
							      vif->active_links, live,
							      link_id);
			if (rid >= 0 && rid != pri_wcid->link_id) {
				struct mt792x_link_sta *live_pri =
					mt792x_sta_to_link(retouch_msta, rid);
				struct ieee80211_link_sta *live_link_sta =
					mt792x_sta_to_link_sta(vif, link_sta->sta, rid);

				if (live_pri && live_link_sta) {
					printk(KERN_INFO
					       "MLO_STA_REC_RETOUCH_SUBJECT: adding_link=%u stock_wcid=%u stock_link=%u live_wcid=%u live_link=%u active=0x%x\n",
					       link_id, pri_wcid->idx, pri_wcid->link_id,
					       live_pri->wcid.idx, rid, vif->active_links);
					pri_mlink = live_pri;
					pri_wcid = &live_pri->wcid;
					mlink->pri_link = live_link_sta;
				}
			}
		}

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_STA_REC_PRIMARY_RETOUCH: adding_link=%u pri_wcid=%u pri_link_id=%u state=ASSOC\n",
			      link_id, pri_wcid->idx, pri_wcid->link_id);

		if (!(READ_ONCE(mt7925_test_partner_after_create) &&
		      is_mt7927(&dev->mt76))) {
			ret = mt7925_partner_retouch(dev, vif, mlink, pri_mlink,
						     false);
			if (ret)
				goto out_pm;
		}

		{
			/* This is the secondary link's OWN first-ever STA_REC
			 * creation (the primary was retouched above; that one
			 * really is just an update to an existing entry). Using
			 * STATE_ASSOC here made mt7925_mcu_sta_update() compute
			 * info.newly = (state != ASSOC) = false, so firmware
			 * received an "update" command for a peer/WTBL entry
			 * that never actually existed yet instead of a real
			 * creation. Proven live: TX worked immediately (the
			 * driver addresses wcid.idx directly, no firmware
			 * lookup needed) but every RX descriptor on this link's
			 * channel resolved to the invalid WLAN_IDX=4095
			 * sentinel forever -- firmware never built the RX-side
			 * peer table because it was never told this was a new
			 * peer. STATE_NONE (matching the primary-link and
			 * non-MLD branches above, both of which ARE genuine
			 * first creations) makes newly=true and fixes this at
			 * the source instead of working around it in the RX
			 * path. This mirrors what mt76_mlo_bssadd_skip==13
			 * already did as a manual debug override below -- that
			 * escape hatch already suspected this exact state was
			 * wrong; make it the real default instead of a hidden
			 * test-only path.
			 */
			enum mt76_sta_info_state secondary_state = MT76_STA_INFO_STATE_NONE;

			if (mt76_mlo_bssadd_skip == 13) {
				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "MLO_STAREC_STATE_OVERRIDE: link_id=%u old_state=%u new_state=%u (now the default; override is a no-op)\n",
					      link_id, MT76_STA_INFO_STATE_ASSOC,
					      MT76_STA_INFO_STATE_NONE);
			}

			if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				      "MLO_STA_REC_SECONDARY: link_id=%u wcid=%u state=%u\n",
				      link_id, mlink->wcid.idx, secondary_state);

			ret = mt7925_mcu_sta_update(dev, link_sta, vif,
						    mlink, true, secondary_state,
						    mlink);
			if (ret)
				goto out_pm;
		}

		if (READ_ONCE(mt7925_test_partner_after_create) &&
		    is_mt7927(&dev->mt76)) {
			ret = mt7925_partner_retouch(dev, vif, mlink, pri_mlink,
						     true);
			if (ret)
				goto out_pm;
		}
	} else {
		ret = mt7925_mcu_sta_update(dev, link_sta, vif,
					    mlink, true,
					    MT76_STA_INFO_STATE_NONE, NULL);
		if (ret)
			goto out_pm;
	}

	mt76_connac_power_save_sched(&dev->mphy, &dev->pm);

	dev_info(dev->mt76.dev,
		 "EXPERIMENTAL MLO TEST: link_id=%u wcid_idx=%u published OK\n",
		 link_id, idx);

	/* This newly created firmware peer supersedes any pending refresh for
	 * an earlier peer incarnation on this BSS. Failed creation never gets
	 * here; it must not erase the unfinished state of a resident peer.
	 */
	if (is_mt7927(&dev->mt76))
		mvif->mt7927_mlo.link[link_id].band_moved = false;

	/* Restored (2026-08-23): stamp this link's readiness time so
	 * mt7925_mlo_link_tx_stable() (mt792x_core.c) can withhold it from
	 * bulk TX for MT7925_MLO_TX_STABILIZE_MS after publish.
	 */
	msta->mlo_link_ready_jiffies[link_id] = jiffies;
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_LINK_READY: link_id=%u mlink=%px wcid_ptr=%px wcid=%u bss=%u omac=%u band=%u hwkey=%d/%d cipher=%u tx_info=0x%x sta_valid=0x%x active_links=0x%x\n",
		      link_id, mlink, &mlink->wcid, idx,
		      mconf ? mconf->mt76.idx : 0xff,
		      mconf ? mconf->mt76.omac_idx : 0xff,
		      mconf ? mconf->mt76.band_idx : 0xff,
		      mlink->wcid.hw_key_idx, mlink->wcid.hw_key_idx2,
		      mlink->wcid.cipher, mlink->wcid.tx_info,
		      msta->valid_links, vif->active_links);

	return 0;

out_pm:
	if (pm_woken)
		mt76_connac_power_save_sched(&dev->mphy, &dev->pm);
out_wcid:
	dev_info(dev->mt76.dev,
		 "EXPERIMENTAL MLO TEST: link_id=%u FAILED ret=%d, unwinding wcid_idx=%u\n",
		 link_id, ret, wcid_published ? wcid->idx : 0xffff);
	if (wcid_published) {
		u16 idx = wcid->idx;

		rcu_assign_pointer(dev->mt76.wcid[idx], NULL);
		mt76_wcid_cleanup(mdev, wcid);
		mt76_wcid_mask_clear(mdev->wcid_mask, wcid->idx);
	}
	return ret;
}

/*
 * Host-only unwind for sta_add_links() failures.
 *
 * If add_links fail due to MCU/firmware timeouts; calling the full remove
 * path would send more firmware commands and may hang again. So only rollback
 * host-published state here (msta->link/valid_links, dev->mt76.wcid[idx]) and
 * free mlink objects (RCU-safe). Firmware state is left for reset/recovery.
 */
static void
mt7925_mac_sta_unwind_links_host(struct mt792x_dev *dev,
				 struct ieee80211_sta *sta,
				 unsigned long links)
{
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	unsigned int link_id;

	for_each_set_bit(link_id, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_link_sta *mlink;
		u16 idx;

		mlink = rcu_replace_pointer(msta->link[link_id], NULL,
					    lockdep_is_held(&dev->mt76.mutex));
		if (!mlink)
			continue;

		msta->valid_links &= ~BIT(link_id);
		if (msta->deflink_id == link_id)
			msta->deflink_id = IEEE80211_LINK_UNSPECIFIED;

		idx = mlink->wcid.idx;
		rcu_assign_pointer(dev->mt76.wcid[idx], NULL);
		mt76_wcid_cleanup(&dev->mt76, &mlink->wcid);
		mt76_wcid_mask_clear(dev->mt76.wcid_mask, idx);

		if (mlink != &msta->deflink)
			kfree_rcu(mlink, rcu_head);
	}
}

struct mt7925_ptk_rebind_ctx {
	struct ieee80211_sta *sta;
	struct mt792x_link_sta *mlink;
	int err;
};

/* TEST-ONLY: when set, mt7925_mlo_ptk_rebind_iter() only traces the match
 * (sta/pairwise/keyidx/cipher/link_id/target WCID) and returns without
 * touching hw_key_idx or programming anything into firmware. For proving
 * the iterator finds the right key/WCID on a stable, untransitioned
 * association before risking a live link add/remove cycle.
 */
static bool mt7925_mlo_ptk_rebind_dryrun;
module_param_named(mlo_ptk_rebind_dryrun, mt7925_mlo_ptk_rebind_dryrun, bool,
		   0644);
MODULE_PARM_DESC(mlo_ptk_rebind_dryrun,
		 "TEST-ONLY: trace PTK rebind matches without programming (default N)");

/* The MLO PTK is per-MLD, installed once via .set_key() when the pairwise
 * key first exists; mac80211 does not call .set_key() again for a link
 * added to the STA afterward (unlike the per-link GTK/IGTK, which it does
 * reissue per link_id). Without this, a link activated after initial
 * association gets a live, LINK_READY WCID that never receives the key
 * mac80211 already negotiated, and stays at hw_key_idx=255 forever.
 *
 * ieee80211_iter_keys() is mac80211's own supported mechanism for a driver
 * to retrieve the keys it currently knows about for a vif/sta pair (its
 * documented use is WoWLAN key reprogramming, which is architecturally the
 * same problem: reinstalling an already-negotiated key onto device state
 * the driver doesn't otherwise have a live pointer to). Using it here means
 * no raw key material is cached by the driver -- the ieee80211_key_conf
 * pointer mac80211 hands back is the same one it already owns and keeps
 * valid until DISABLE_KEY, so this is safe for exactly as long as the key
 * itself is.
 */
static void mt7925_mlo_ptk_rebind_iter(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif,
				       struct ieee80211_sta *sta,
				       struct ieee80211_key_conf *key,
				       void *data)
{
	struct mt7925_ptk_rebind_ctx *ctx = data;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_bss_conf *mconf;
	struct mt792x_sta *msta;
	struct mt76_wcid *wcid;

	if (sta != ctx->sta || !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE))
		return;

	msta = (struct mt792x_sta *)sta->drv_priv;
	wcid = &ctx->mlink->wcid;

	if (READ_ONCE(mt7925_mlo_ptk_rebind_dryrun)) {
		dev_info(dev->mt76.dev,
			"MLO_PTK_REBIND_DRYRUN: sta_match=1 pairwise=1 keyidx=%d cipher=0x%x key_link_id=%d target_link_id=%u target_wcid=%u cur_hw_key_idx=%d\n",
			key->keyidx, key->cipher, key->link_id,
			wcid->link_id, wcid->idx, wcid->hw_key_idx);
		return;
	}

	if (wcid->hw_key_idx == key->keyidx)
		return;

	/* Parity with mt7925_set_link_key(): the new link's own per-link BSS
	 * context (not the link the key was originally installed against)
	 * has never had its cipher recorded, since it didn't exist at the
	 * original SET_KEY time.
	 */
	mconf = mt792x_vif_to_link(mvif, wcid->link_id);
	if (mconf && !mconf->mt76.cipher) {
		struct ieee80211_bss_conf *link_conf =
			mt792x_vif_to_bss_conf(vif, wcid->link_id);
		struct ieee80211_link_sta *link_sta =
			rcu_dereference_protected(sta->link[wcid->link_id],
				lockdep_is_held(&dev->mt76.mutex));

		mconf->mt76.cipher = mt7925_mcu_get_cipher(key->cipher);
		if (link_conf)
			mt7925_mcu_add_bss_info(&dev->phy, mconf->mt76.ctx,
						link_conf, link_sta, true);
	}

	/* Transactional, matching mt7925_mlo_group_key_rebind_iter():
	 * mt76_wcid_key_setup() must still run before mt7925_mcu_add_key()
	 * (it builds the cipher/PN-check state the MCU payload is
	 * constructed from), but only the idempotency-visible hw_key_idx is
	 * rolled back on failure -- publishing it unconditionally let a
	 * failed firmware install look identical to a real one on the next
	 * call's shortcut check above, permanently skipping the retry.
	 */
	{
		u8 prev_hw_key_idx = wcid->hw_key_idx;

		wcid->hw_key_idx = key->keyidx;
		mt76_wcid_key_setup(&dev->mt76, wcid, key);
		ctx->err = mt7925_mcu_add_key(&dev->mt76, vif, &ctx->mlink->bip,
					      key, MCU_UNI_CMD(STA_REC_UPDATE),
					      wcid, SET_KEY, msta);
		if (ctx->err)
			wcid->hw_key_idx = prev_hw_key_idx;
	}
}

int mt7925_mlo_ptk_rebind(struct mt792x_dev *dev,
				 struct ieee80211_vif *vif,
				 struct ieee80211_sta *sta,
				 struct mt792x_link_sta *mlink)
{
	struct mt7925_ptk_rebind_ctx ctx = { .sta = sta, .mlink = mlink };

	if (!ieee80211_vif_is_mld(vif))
		return 0;

	ieee80211_iter_keys(mt76_hw(dev), vif, mt7925_mlo_ptk_rebind_iter,
			    &ctx);

	if (ctx.err)
		dev_info(dev->mt76.dev,
			"MLO PTK rebind failed: link_id=%u wcid=%u ret=%d\n",
			mlink->wcid.link_id, mlink->wcid.idx, ctx.err);

	return ctx.err;
}

struct mt7925_group_key_rebind_ctx {
	struct mt792x_vif *mvif;
	unsigned int link_id;
	unsigned int keys_seen;
	unsigned int gtk_seen;
	unsigned int igtk_seen;
	unsigned int bigtk_seen;
	unsigned int already_current;
	unsigned int installs_attempted;
	unsigned int installs_succeeded;
	int first_err;
};

/* Group (GTK/IGTK/BIGTK) counterpart of mt7925_mlo_ptk_rebind_iter(). The
 * group key is per-link (mac80211 does reissue SET_KEY per link_id for
 * these, unlike the per-MLD pairwise key), but a link added AFTER the
 * original SET_KEY calls still needs its own BMC WCID (mvif->sta.link[]  --
 * the same GROUP/mvif-sta pseudo-STA mt7925_set_link_key() already targets
 * for cmd=NULL sta) programmed with whatever group keys mac80211 currently
 * owns for it, exactly like the pairwise case. Matched the same way
 * mt7925_set_key()'s own group branch does: key->link_id == -1 means "all
 * links", otherwise it must equal this link.
 */
static void mt7925_mlo_group_key_rebind_iter(struct ieee80211_hw *hw,
					     struct ieee80211_vif *vif,
					     struct ieee80211_sta *sta,
					     struct ieee80211_key_conf *key,
					     void *data)
{
	struct mt7925_group_key_rebind_ctx *ctx = data;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_vif *mvif = ctx->mvif;
	struct mt792x_link_sta *mlink;
	struct mt792x_bss_conf *mconf;
	struct mt76_wcid *wcid;
	u8 *wcid_keyidx;
	u8 prev_keyidx;
	int err;

	if (sta || (key->flags & IEEE80211_KEY_FLAG_PAIRWISE))
		return;
	if (key->link_id != -1 && key->link_id != ctx->link_id)
		return;

	mlink = mt792x_sta_to_link(&mvif->sta, ctx->link_id);
	if (!mlink)
		return;

	ctx->keys_seen++;

	/* Classify by cipher/keyidx for separate GTK/IGTK/BIGTK diagnostics
	 * only -- this never changes which key gets installed or how, it
	 * only labels the counters a caller (postkey key-readiness gate)
	 * reports. Per 802.11, BIP_* ciphers protect management frames:
	 * keyidx 4-5 is IGTK, 6-7 is BIGTK; anything else non-pairwise here
	 * is a data GTK.
	 */
	if (key->cipher == WLAN_CIPHER_SUITE_AES_CMAC ||
	    key->cipher == WLAN_CIPHER_SUITE_BIP_CMAC_256 ||
	    key->cipher == WLAN_CIPHER_SUITE_BIP_GMAC_128 ||
	    key->cipher == WLAN_CIPHER_SUITE_BIP_GMAC_256) {
		if (key->keyidx >= 6)
			ctx->bigtk_seen++;
		else
			ctx->igtk_seen++;
	} else {
		ctx->gtk_seen++;
	}

	wcid = &mlink->wcid;
	wcid_keyidx = key->cipher == WLAN_CIPHER_SUITE_AES_CMAC ?
		      &wcid->hw_key_idx2 : &wcid->hw_key_idx;

	printk(KERN_INFO "MLO_GROUP_KEY_CHECK: link_id=%u keyidx=%d cipher=0x%x wcid=%u cur_idx=%d shortcut=%d\n",
	       ctx->link_id, key->keyidx, key->cipher, wcid->idx,
	       *wcid_keyidx, *wcid_keyidx == key->keyidx);

	if (*wcid_keyidx == key->keyidx) {
		ctx->already_current++;
		return;
	}

	mconf = mt792x_vif_to_link(mvif, ctx->link_id);
	if (mconf && !mconf->mt76.cipher) {
		struct ieee80211_bss_conf *link_conf =
			mt792x_vif_to_bss_conf(vif, ctx->link_id);

		mconf->mt76.cipher = mt7925_mcu_get_cipher(key->cipher);
		if (link_conf)
			mt7925_mcu_add_bss_info(&dev->phy, mconf->mt76.ctx,
						link_conf, NULL, true);
	}

	/* mt76_wcid_key_setup() runs before the MCU send everywhere else in
	 * this driver (mt7925_set_link_key() does the same) because
	 * mt7925_mcu_add_key() reads the cipher/PN-check state it sets up on
	 * `wcid` to build the MCU payload -- deferring it would change what
	 * gets sent, not just when the success is reported. Only the
	 * idempotency-visible *wcid_keyidx must wait for real MCU success:
	 * publishing it early is what let a failed install look identical to
	 * a real one on the next call's shortcut check.
	 */
	prev_keyidx = *wcid_keyidx;
	mt76_wcid_key_setup(&dev->mt76, wcid, key);

	ctx->installs_attempted++;
	err = mt7925_mcu_add_key(&dev->mt76, vif, &mlink->bip, key,
				 MCU_UNI_CMD(STA_REC_UPDATE), wcid,
				 SET_KEY, &mvif->sta);
	if (err) {
		*wcid_keyidx = prev_keyidx;
		if (!ctx->first_err)
			ctx->first_err = err;
		return;
	}

	*wcid_keyidx = key->keyidx;
	ctx->installs_succeeded++;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_GROUP_KEY_REBIND: link_id=%u keyidx=%d cipher=0x%x wcid=%u hw_key_idx=%d hw_key_idx2=%d\n",
		       ctx->link_id, key->keyidx, key->cipher, wcid->idx,
		       wcid->hw_key_idx, wcid->hw_key_idx2);
}

int mt7925_mlo_group_key_rebind(struct mt792x_dev *dev,
				struct ieee80211_vif *vif,
				unsigned int link_id,
				struct mt7925_group_key_rebind_result *result)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt7925_group_key_rebind_ctx ctx = { .mvif = mvif, .link_id = link_id };

	if (!ieee80211_vif_is_mld(vif))
		return 0;

	ieee80211_iter_keys(mt76_hw(dev), vif, mt7925_mlo_group_key_rebind_iter,
			    &ctx);

	dev_info(dev->mt76.dev,
		"MLO_GROUP_KEY_REBIND_SUMMARY: link_id=%u keys_seen=%u gtk=%u igtk=%u bigtk=%u already_current=%u installs_attempted=%u installs_succeeded=%u first_err=%d\n",
		link_id, ctx.keys_seen, ctx.gtk_seen, ctx.igtk_seen,
		ctx.bigtk_seen, ctx.already_current, ctx.installs_attempted,
		ctx.installs_succeeded, ctx.first_err);

	if (result) {
		result->keys_seen = ctx.keys_seen;
		result->gtk_seen = ctx.gtk_seen;
		result->igtk_seen = ctx.igtk_seen;
		result->bigtk_seen = ctx.bigtk_seen;
		result->already_current = ctx.already_current;
		result->installs_attempted = ctx.installs_attempted;
		result->installs_succeeded = ctx.installs_succeeded;
	}

	return ctx.first_err;
}

/* Orchestrator: given a peer link whose WCID/BSS/STA_REC identity is now
 * stable (LINK_READY), reconstruct every already-existing MLD security
 * dependency that link needs to carry real traffic. Composes the two
 * existing per-key-type rebind primitives above in the required order
 * (unicast PTK, then group/BMC) -- it does not reimplement their logic.
 * Both are already idempotent (each no-ops when the WCID's hw_key_idx
 * already matches), so calling this unconditionally on every link add,
 * initial or late, is safe: it only ever does real work for a link that
 * actually needs the catch-up mac80211 won't otherwise trigger.
 */
int mt7925_mlo_reconcile_link(struct mt792x_dev *dev,
				     struct ieee80211_vif *vif,
				     struct ieee80211_sta *sta,
				     struct mt792x_link_sta *mlink,
				     unsigned int link_id)
{
	int err;

	if (!ieee80211_vif_is_mld(vif))
		return 0;

	err = mt7925_mlo_ptk_rebind(dev, vif, sta, mlink);
	if (err)
		return err;

	return mt7925_mlo_group_key_rebind(dev, vif, link_id, NULL);
}

int mt7925_mac_link_sta_assoc(struct mt792x_dev *dev,
			       struct ieee80211_vif *vif,
			       struct ieee80211_link_sta *link_sta);

int
mt7925_mac_sta_add_links(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta, unsigned long new_links)
{
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	unsigned long added_links = 0;
	unsigned int link_id;
	int err = 0;

	for_each_set_bit(link_id, &new_links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_link_sta *link_sta;
		struct mt792x_link_sta *mlink;
		bool is_deflink = false;

		if (msta->deflink_id == IEEE80211_LINK_UNSPECIFIED) {
			mlink = &msta->deflink;
			is_deflink = true;
		} else {
			mlink = kzalloc(sizeof(*mlink), GFP_KERNEL);
			if (!mlink) {
				err = -ENOMEM;
				break;
			}
		}

		mlink->sta = msta;
		link_sta = mt792x_sta_to_link_sta(vif, sta, link_id);
		if (!link_sta) {
			if (!is_deflink)
				kfree_rcu(mlink, rcu_head);
			err = -EINVAL;
			break;
		}

		if (is_deflink ||
		    msta->deflink_id == IEEE80211_LINK_UNSPECIFIED) {
			mlink->pri_link = &sta->deflink;
		} else {
			mlink->pri_link = mt792x_sta_to_link_sta(vif, sta,
								 msta->deflink_id);
			if (!mlink->pri_link)
				mlink->pri_link = &sta->deflink;
		}
		if (is_deflink) {
			mlink->wcid.def_wcid = NULL;
		} else {
			mlink->wcid.def_wcid = &msta->deflink.wcid;
		}

		err = mt7925_mac_link_sta_add(&dev->mt76, vif, link_sta, mlink);
		if (err) {
			if (!is_deflink)
				kfree_rcu(mlink, rcu_head);
			break;
		}

		if (is_deflink)
			msta->deflink_id = link_id;

		rcu_assign_pointer(msta->link[link_id], mlink);
		msta->valid_links |= BIT(link_id);

		if (!is_deflink) {
			err = mt7925_mcu_mld_presetup(dev, vif, sta, link_id);
			if (err) {
				added_links |= BIT(link_id);
				break;
			}
		}

		/* From this point the host-published link must be unwound on
		 * any failed key or firmware admission operation.
		 */
		added_links |= BIT(link_id);
		if (is_deflink || !READ_ONCE(mt7925_test_mt7927_ptk_after_mld)) {
			err = mt7925_mlo_reconcile_link(dev, vif, sta, mlink, link_id);
			if (err)
				break;
		}

		/* Race fix: mt7925_mac_sta_event() (MT76_STA_EVENT_ASSOC,
		 * mac80211's real "peer is port-secure, admit to TX" signal)
		 * fires exactly once per STA and only re-admits whichever
		 * links were already in msta->valid_links at that instant.
		 * When a link of the SAME initial MLO negotiation finishes
		 * being added to this STA slightly after that event already
		 * fired (real, live trace: primary added at valid_links=0x4,
		 * secondary added moments later at valid_links=0x7, ASSOC
		 * event having already run in between), this link never gets
		 * admitted at all -- same "installed/keyed but zero real
		 * airtime" signature as the original secondary-admission bug,
		 * just reached via a timing race instead of every time. Give
		 * every non-deflink link its own explicit admission here,
		 * unconditionally: by this point in mt7925_mac_sta_add_links()
		 * the per-MLD PTK is already installed for any link added
		 * after the STA's own initial handshake (which is exactly the
		 * case this race hits), so it is safe, and the call is
		 * idempotent for the ASSOC-event path re-covering the same
		 * link.
		 */
		if (!is_deflink) {
			struct ieee80211_link_sta *assoc_link_sta =
				mt792x_sta_to_link_sta(vif, sta, link_id);

			if (!assoc_link_sta) {
				err = -EINVAL;
				break;
			}
			err = mt7925_mac_link_sta_assoc(dev, vif, assoc_link_sta);
			if (err)
				break;
			if (READ_ONCE(mt7925_test_mt7927_ptk_after_mld)) {
				err = mt7925_mlo_reconcile_link(dev, vif, sta, mlink,
							     link_id);
				if (err)
					break;
			}
		}

		added_links |= BIT(link_id);
	}

	if (err && added_links)
		mt7925_mac_sta_unwind_links_host(dev, sta, added_links);

	return err;
}

int mt7925_mac_sta_add(struct mt76_dev *mdev, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	int err;

	msta->vif = mvif;

	wiphy_work_init(&msta->postkey_stage1, mt7927_mlo_postkey_stage1_work);
	INIT_WORK(&msta->postkey_stage2, mt7927_mlo_postkey_stage2_work);

	if (vif->type == NL80211_IFTYPE_STATION)
		mvif->wep_sta = msta;

	if (ieee80211_vif_is_mld(vif)) {
		msta->deflink_id = IEEE80211_LINK_UNSPECIFIED;

		err = mt7925_mac_sta_add_links(dev, vif, sta, sta->valid_links);
	} else {
		err = mt7925_mac_link_sta_add(mdev, vif, &sta->deflink,
					      &msta->deflink);
	}

	return err;
}
EXPORT_SYMBOL_GPL(mt7925_mac_sta_add);

/* Teardown of a deferred assoc-time JOIN that never ran. */
void mt7925_mlo_join_defer_drop(struct mt792x_vif *mvif, const char *why)
{
	if (!mvif->mt7927_mlo.join_deferred)
		return;
	printk(KERN_INFO
	       "MLO_JOIN_DEFER_DROPPED: reason=%s wanted_links=0x%x gen=%u cur_gen=%u\n",
	       why, mvif->mt7927_mlo.join_deferred_links,
	       mvif->mt7927_mlo.join_deferred_gen,
	       mvif->mt7927_mlo.transition_generation);
	mvif->mt7927_mlo.join_deferred = false;
	mvif->mt7927_mlo.join_deferred_links = 0;
}

static int mt7925_mac_set_links(struct mt76_dev *mdev, struct ieee80211_vif *vif);

/* Manual resume of a held activation; caller holds the wiphy lock. */
int mt7925_mlo_activate_resume(struct mt792x_dev *dev, struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;

	if (!mvif->mt7927_mlo.activate_deferred || !ieee80211_vif_is_mld(vif))
		return -ENOENT;
	printk(KERN_INFO "MLO_ACTIVATE_DEFER_RESUME: manual held_ms=%u\n",
	       jiffies_to_msecs(jiffies - mvif->mt7927_mlo.activate_deferred_jiffies));
	mvif->mt7927_mlo.activate_deferred = false;
	return mt7925_mac_set_links(&dev->mt76, vif);
}

void mt7925_mlo_activate_defer_drop(struct mt792x_vif *mvif, const char *why)
{
	if (!mvif->mt7927_mlo.activate_deferred)
		return;
	printk(KERN_INFO "MLO_ACTIVATE_DEFER_DROPPED: reason=%s held_ms=%u\n", why,
	       jiffies_to_msecs(jiffies - mvif->mt7927_mlo.activate_deferred_jiffies));
	mvif->mt7927_mlo.activate_deferred = false;
}

static int
mt7925_mac_set_links(struct mt76_dev *mdev, struct ieee80211_vif *vif)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_bss_conf *link_conf =
		mt792x_vif_to_bss_conf(vif, mvif->deflink_id);
	struct cfg80211_chan_def *chandef = &link_conf->chanreq.oper;
	enum nl80211_band band = chandef->chan->band, secondary_band;
	u16 sel_links = mt76_select_links(vif, 2);
	u16 test_pair = READ_ONCE(mt7925_test_mlo_pair_mask);
	unsigned long usable_links = ieee80211_vif_usable_links(vif);
	u8 secondary_link_id;
	bool explicit_pair = false;
	u16 auto_links = sel_links;	/* the original choice, before the second caller */
	bool override_taken = false;
	int err;

	if (test_pair && is_mt7927(mdev)) {
		if (test_pair != 0x3 && test_pair != 0x5 && test_pair != 0x6) {
			dev_warn(dev->mt76.dev,
				 "MLO_LINK_SELECT: TEST-ONLY MT7927 pair 0x%x invalid; automatic pair remains 0x%x\n",
				 test_pair, sel_links);
		} else if ((test_pair & usable_links) != test_pair) {
			dev_warn(dev->mt76.dev,
				 "MLO_LINK_SELECT: TEST-ONLY MT7927 pair 0x%x not usable (candidates=0x%lx); automatic pair remains 0x%x\n",
				 test_pair, usable_links, sel_links);
		} else if (!(test_pair & BIT(mvif->deflink_id))) {
			dev_warn(dev->mt76.dev,
				 "MLO_LINK_SELECT: TEST-ONLY MT7927 pair 0x%x excludes active anchor link%u; automatic pair remains 0x%x\n",
				 test_pair, mvif->deflink_id, sel_links);
		} else {
			sel_links = test_pair;
			explicit_pair = true;
			dev_warn(dev->mt76.dev,
				 "MLO_LINK_SELECT: TEST-ONLY MT7927 EHT/MLO pair override selected pair=0x%x anchor=link%u candidates=0x%lx\n",
				 sel_links, mvif->deflink_id, usable_links);
		}
	}

	/* Second caller: after a recovery reset the previous connection is severe;
	 * the automatic pair must not walk back into it. Only an override that
	 * keeps the association anchor and is a real pair is taken.
	 */
	if (!explicit_pair && is_mt7927(mdev)) {
		u16 decided = mt7925_mlo_severity_decide(vif, usable_links);

		if (decided && hweight16(decided) == 2 &&
		    (decided & BIT(mvif->deflink_id)) && decided != sel_links) {
			dev_info(dev->mt76.dev,
				 "MLO_SEVERITY: pair 0x%x replaces automatic 0x%x (previous connection severe)\n",
				 decided, sel_links);
			sel_links = decided;
			override_taken = true;
		} else if (decided) {
			dev_info(dev->mt76.dev,
				 "MLO_SEVERITY: decision 0x%x not taken (anchor link%u, automatic 0x%x)\n",
				 decided, mvif->deflink_id, sel_links);
		}
	}

retry:
	if (!ieee80211_vif_is_mld(vif) || hweight16(sel_links) < 2)
		return 0;

	/* Admission precedes every firmware reservation mutation. An explicit
	 * diagnostic pair never falls back; automatic choice may use another
	 * enabled, usable pair that retains the actual association anchor.
	 */
	if (is_mt7927(mdev) && !explicit_pair &&
	    !mt7927_mlo_pair_enabled(vif, sel_links)) {
		static const u16 pairs[] = { 0x3, 0x5, 0x6 };
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(pairs); i++) {
			if ((pairs[i] & usable_links) != pairs[i] ||
			    !(pairs[i] & BIT(mvif->deflink_id)) ||
			    !mt7927_mlo_pair_enabled(vif, pairs[i]))
				continue;
			sel_links = pairs[i];
			break;
		}
	}
	err = mt7927_mlo_pairs_prepare_association(vif, sel_links);
	if (err) {
		mt7927_mlo_pairs_note(vif, sel_links, err,
				      "association_admission", false);
		goto fail;
	}

	secondary_link_id = __ffs(~BIT(mvif->deflink_id) & sel_links);

	link_conf = mt792x_vif_to_bss_conf(vif, secondary_link_id);
	secondary_band = link_conf->chanreq.oper.chan->band;

	/* Also while the partner's context is not installed: a link config that
	 * survives an earlier shrink has no BSS in the firmware view, and the
	 * request builder refuses the JOIN for it.
	 */
	if (READ_ONCE(mt7925_mlo_join_wait_bss) && is_mt7927(mdev) &&
	    (!rcu_access_pointer(mvif->link_conf[secondary_link_id]) ||
	     !mt7927_mlo_link_ctx_installed(dev, mvif, secondary_link_id))) {
		mvif->mt7927_mlo.join_deferred = true;
		mvif->mt7927_mlo.join_deferred_links = sel_links;
		mvif->mt7927_mlo.join_deferred_gen =
			mvif->mt7927_mlo.transition_generation;
		dev_info(dev->mt76.dev,
			 "MLO_JOIN_DEFER_NO_BSS: anchor=%u secondary=%u sel_links=0x%x gen=%u partner BSS not registered; JOIN left to change_vif_links\n",
			 mvif->deflink_id, secondary_link_id, sel_links,
			 mvif->mt7927_mlo.transition_generation);
	} else if (band == NL80211_BAND_2GHZ ||
	    (band == NL80211_BAND_5GHZ && secondary_band == NL80211_BAND_6GHZ)) {
		err = mt7925_abort_roc(mvif->phy, &mvif->bss_conf);
		if (err) {
			mt7927_mlo_pairs_note(vif, sel_links, err,
					      "association_abort", false);
			goto fail;
		}

		mt792x_mutex_acquire(dev);
		err = mt7925_set_mlo_roc(mvif->phy, &mvif->bss_conf, sel_links);
		mt792x_mutex_release(dev);

		if (err) {
			mt7927_mlo_pairs_note(vif, sel_links, err,
					      "association_join", false);
			dev_warn(dev->mt76.dev,
				 "MLO ROC reservation failed (err=%d), not activating link 0x%x\n",
				 err, sel_links);
			goto fail;
		}
	}

	/* STA ASSOC precedes publication of cfg.assoc in mac80211. Preserve
	 * its initial asynchronous activation after administrative admission;
	 * ordinary associated requests still use the strict completion gate.
	 */
	if (!vif->cfg.assoc) {
		mt7927_mlo_pairs_note(vif, sel_links, 0,
				      "association_activation_request", false);
		ieee80211_set_active_links_async(vif, sel_links);
	} else {
		err = mt7925_mlo_request_active_links(dev, vif, sel_links);
		if (err)
			goto fail;
	}

	if (is_mt7927(mdev)) {
		unsigned long dormant = vif->valid_links & ~sel_links;
		unsigned int dl;

		mt792x_mutex_acquire(dev);
		for_each_set_bit(dl, &dormant, IEEE80211_MLD_MAX_NUM_LINKS) {
			int derr = mt7927_mlo_prime_dormant_link(dev, vif, mvif, dl);

			if (derr)
				dev_warn(dev->mt76.dev,
					 "MLO_DORMANT_BSS: link_id=%u prime failed err=%d\n",
					 dl, derr);
		}
		mt792x_mutex_release(dev);
	}
	return 0;

fail:
	/* The second caller's pair did not come up: fall back to the original
	 * connection (the automatic pair) once, instead of failing the association.
	 */
	if (override_taken) {
		dev_warn(dev->mt76.dev,
			 "MLO_SEVERITY: pair 0x%x failed (err=%d); falling back to the original pair 0x%x\n",
			 sel_links, err, auto_links);
		override_taken = false;
		sel_links = auto_links;
		goto retry;
	}
	return err;
}


int mt7925_mac_sta_event(struct mt76_dev *mdev, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta, enum mt76_sta_event ev)
{
	struct ieee80211_link_sta *link_sta = &sta->deflink;

	/* Fires on the sta_state ASSOC->AUTH decreasing transition, which
	 * __sta_info_destroy_part2() (mac80211 core) walks through
	 * unconditionally, strictly before its later drv_sta_state(NOTEXIST)
	 * call and cleanup_single_sta()/sta_info_free() -- msta is
	 * guaranteed live here. It also runs with the wiphy mutex already
	 * held (this whole destroy sequence does) and dev->mt76.mutex NOT
	 * yet held by this function (unlike the ASSOC case below, which only
	 * takes it internally, per-call, inside
	 * mt7925_mac_link_sta_assoc_locked()) -- so cancelling here, before
	 * this function ever touches dev->mt76.mutex, cannot deadlock
	 * against stage2 (which needs that mutex to run and return).
	 */
	if (ev == MT76_STA_EVENT_DISASSOC) {
		struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev,
						      mt76);
		struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "POSTKEY_KEEP_DORMANT: reason=sta_disassoc pending=%d sublink=%u survivor=%u gen=%u\n",
			      msta->postkey_pending, msta->postkey_sublink_id,
			      msta->postkey_survivor_id,
			      msta->postkey_generation);

		wiphy_work_cancel(mt76_hw(dev)->wiphy, &msta->postkey_stage1);
		/* A TDLS peer can be removed by active-link switching itself;
		 * that must not invalidate the infrastructure association ticket.
		 */
		if (vif->type == NL80211_IFTYPE_STATION && !sta->tdls &&
		    ether_addr_equal(sta->addr, vif->cfg.ap_addr)) {
			mt7925_mlo_join_defer_drop((struct mt792x_vif *)vif->drv_priv,
						   "sta_disassoc");
			mt7925_mlo_activate_defer_drop((struct mt792x_vif *)vif->drv_priv,
						       "sta_disassoc");
			mt7925_mlo_completion_stop(vif);
		}
		cancel_work_sync(&msta->postkey_stage2);
		mt7927_mlo_pending_clear(msta);
		return 0;
	}

	if (ev == MT76_STA_EVENT_AUTHORIZE) {
		struct mt792x_vif *amvif = (struct mt792x_vif *)vif->drv_priv;

		if (!sta->tdls && ether_addr_equal(sta->addr, vif->cfg.ap_addr)) {
			mt7927_mlo_pairs_authorized(vif);
			mt7927_mlo_steering_kick(vif);
		}

		if (amvif->mt7927_mlo.activate_deferred &&
		    READ_ONCE(mt7925_mlo_activate_hold)) {
			printk(KERN_INFO "MLO_ACTIVATE_HOLD: authorized, activation kept held (debugfs mlo_activate_now)\n");
		} else if (amvif->mt7927_mlo.activate_deferred &&
		    ieee80211_vif_is_mld(vif)) {
			printk(KERN_INFO "MLO_ACTIVATE_DEFER_RESUME: held_ms=%u\n",
			       jiffies_to_msecs(jiffies -
						amvif->mt7927_mlo.activate_deferred_jiffies));
			amvif->mt7927_mlo.activate_deferred = false;
			return mt7925_mac_set_links(mdev, vif);
		}
		return 0;
	}

	if (ev != MT76_STA_EVENT_ASSOC)
		return 0;

	if (ieee80211_vif_is_mld(vif)) {
		struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
		unsigned long valid_links = msta->valid_links;
		unsigned int link_id;

		/* mac80211's AUTH->ASSOC transition fires once for the whole
		 * MLD STA, after the real (per-MLD) PTK is installed -- this
		 * is the actual "peer is now port-secure, admit its WCID to
		 * real TX" signal (mt7925_mac_link_sta_assoc() below sends
		 * CONN_STATE_PORT_SECURE + STA_REC state=ASSOC). Resolving
		 * only msta->deflink_id here meant every OTHER already-valid
		 * link's WCID was created/keyed but never got this admission
		 * command at all: live trace proved a secondary link's WCID
		 * stays fully "installed" (valid, keyed, STA_REC ret=0) while
		 * firmware never gives it real airtime (tx_pkts increments,
		 * cum_tx_airtime_us stays 0, rx_pkts stays 0) -- on a
		 * completely fresh association, no remove/re-add involved.
		 * Send the same admission to every currently valid link.
		 */
		if (READ_ONCE(mt7925_mlo_activate_after_auth) &&
		    is_mt7927(mdev) && vif->type == NL80211_IFTYPE_STATION) {
			struct mt792x_vif *amvif = (struct mt792x_vif *)vif->drv_priv;

			amvif->mt7927_mlo.activate_deferred = true;
			amvif->mt7927_mlo.activate_deferred_jiffies = jiffies;
			printk(KERN_INFO
			       "MLO_ACTIVATE_DEFER: link activation held until STA authorize (valid=0x%x active=0x%x)\n",
			       vif->valid_links, vif->active_links);
		} else {
			int err = mt7925_mac_set_links(mdev, vif);

			if (err)
				return err;
		}

		for_each_set_bit(link_id, &valid_links,
				 IEEE80211_MLD_MAX_NUM_LINKS) {
			int err;

			link_sta = mt792x_sta_to_link_sta(vif, sta, link_id);
			if (!link_sta)
				continue;

			err = mt7925_mac_link_sta_assoc_locked(mdev, vif, link_sta);

			if (err)
				return err;
		}

		return 0;
	}

	return mt7925_mac_link_sta_assoc_locked(mdev, vif, link_sta);
}
EXPORT_SYMBOL_GPL(mt7925_mac_sta_event);


int
mt7925_mac_sta_remove_links(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			    struct ieee80211_sta *sta, unsigned long old_links)
{
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	struct mt76_dev *mdev = &dev->mt76;
	unsigned long remaining_links;
	unsigned int link_id;

	remaining_links = msta->valid_links & ~old_links;
	if (msta->deflink_id != IEEE80211_LINK_UNSPECIFIED &&
	    (old_links & BIT(msta->deflink_id))) {
		if (remaining_links)
			msta->deflink_id = __ffs(remaining_links);
		else
			msta->deflink_id = IEEE80211_LINK_UNSPECIFIED;
	}

	/* clean up bss before starec */
	for_each_set_bit(link_id, &old_links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_link_sta *link_sta;
		struct ieee80211_bss_conf *link_conf;
		struct mt792x_bss_conf *mconf;
		struct mt792x_link_sta *mlink;

		if (vif->type == NL80211_IFTYPE_AP)
			break;

		if (vif->type == NL80211_IFTYPE_STATION && sta->tdls)
			continue;

		link_sta = mt792x_sta_to_link_sta(vif, sta, link_id);
		if (!link_sta)
			continue;

		mlink = mt792x_sta_to_link(msta, link_id);
		if (!mlink)
			continue;

		link_conf = mt792x_vif_to_bss_conf(vif, link_id);
		if (!link_conf)
			continue;

		mconf = mt792x_link_conf_to_mconf(link_conf);

		mt7925_mcu_add_bss_info(&dev->phy, mconf->mt76.ctx, link_conf,
					link_sta, false);
	}

	for_each_set_bit(link_id, &old_links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_link_sta *link_sta;
		struct mt792x_link_sta *mlink;

		link_sta = mt792x_sta_to_link_sta(vif, sta, link_id);
		if (!link_sta)
			continue;

		mlink = rcu_replace_pointer(msta->link[link_id], NULL,
					    lockdep_is_held(&mdev->mutex));
		if (!mlink)
			continue;

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)) {
			struct ieee80211_bss_conf *link_conf;
			struct mt792x_bss_conf *mconf = NULL;

			link_conf = mt792x_vif_to_bss_conf(vif, link_id);
			if (link_conf)
				mconf = mt792x_link_conf_to_mconf(link_conf);
			printk(KERN_INFO
			       "MLO_WCID_LIFE: stage=remove_before link=%u mlink=%px wcid=%px idx=%u mconf=%px bss=%u omac=%u band=%u phy=%u hwkey=%d/%d cipher=%u tx_info=0x%x valid=%u sta_valid=0x%x deflink_id=%u\n",
			       link_id, mlink, &mlink->wcid, mlink->wcid.idx,
			       mconf, mconf ? mconf->mt76.idx : 0xff,
			       mconf ? mconf->mt76.omac_idx : 0xff,
			       mconf ? mconf->mt76.band_idx : 0xff,
			       mlink->wcid.phy_idx, mlink->wcid.hw_key_idx,
			       mlink->wcid.hw_key_idx2, mlink->wcid.cipher,
			       mlink->wcid.tx_info, mlink->wcid.link_valid,
			       msta->valid_links, msta->deflink_id);
		}

		msta->valid_links &= ~BIT(link_id);

		/* mt7925_mac_sta_poll() (NAPI/RX-poll context, runs on any CPU
		 * against dev->mt76.sta_poll_list with no relation to this
		 * mutex) reads mlink->sta unconditionally before its own
		 * wcid.sta gate. Clearing mlink->sta here while this mlink can
		 * still be sitting on that global list -- true until
		 * mt7925_mac_link_sta_remove() gets around to it below -- lets
		 * that poll pick up a NULL msta and container_of() it into a
		 * bogus near-NULL ieee80211_sta pointer, which mac80211 then
		 * dereferences and crashes on. Proven live: page fault in
		 * ieee80211_sta_register_airtime() from exactly this path,
		 * full oops trace, right after a link removal. Take this
		 * mlink off the poll list first, under the same lock
		 * mt7925_mac_sta_poll() uses, so no poll iteration can ever
		 * observe it between "unlinked from msta" and "removed from
		 * firmware".
		 */
		spin_lock_bh(&dev->mt76.sta_poll_lock);
		if (!list_empty(&mlink->wcid.poll_list))
			list_del_init(&mlink->wcid.poll_list);
		spin_unlock_bh(&dev->mt76.sta_poll_lock);

		mlink->sta = NULL;
		mlink->pri_link = NULL;

		mt7925_mac_link_sta_remove(&dev->mt76, vif, link_sta, mlink);

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			       "MLO_WCID_LIFE: stage=remove_after link=%u mlink=%px wcid=%px old_idx=%u sta_valid=0x%x deflink_id=%u\n",
			       link_id, mlink, &mlink->wcid, mlink->wcid.idx,
			       msta->valid_links, msta->deflink_id);

		if (mlink != &msta->deflink)
			kfree_rcu(mlink, rcu_head);

		if (!remaining_links && msta->deflink_id == link_id)
			msta->deflink_id = IEEE80211_LINK_UNSPECIFIED;
	}

	return 0;
}

void mt7925_mac_sta_remove(struct mt76_dev *mdev, struct ieee80211_vif *vif,
			   struct ieee80211_sta *sta)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;

	if (ieee80211_vif_is_mld(vif)) {
		mt7925_mac_sta_remove_links(dev, vif, sta, msta->valid_links);
		mt7925_mcu_del_dev(mdev, vif);
	} else {
		mt7925_mac_link_sta_remove(mdev, vif, &sta->deflink,
					   &msta->deflink);
	}

	if (vif->type == NL80211_IFTYPE_STATION) {
		mvif->wep_sta = NULL;
		ewma_rssi_init(&mvif->bss_conf.rssi);
	}

	mvif->mlo_pm_state = MT792x_MLO_LINK_DISASSOC;
}
EXPORT_SYMBOL_GPL(mt7925_mac_sta_remove);

static void
mt7925_mlo_pm_iter(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt792x_dev *dev = priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long valid = ieee80211_vif_is_mld(vif) ?
				    mvif->valid_links : BIT(0);
	struct ieee80211_bss_conf *bss_conf;
	int i;

	if (mvif->mlo_pm_state != MT792x_MLO_CHANGED_PS)
		return;

	mt792x_mutex_acquire(dev);
	for_each_set_bit(i, &valid, IEEE80211_MLD_MAX_NUM_LINKS) {
		bss_conf = mt792x_vif_to_bss_conf(vif, i);
		mt7925_mcu_uni_bss_ps(dev, bss_conf);
	}
	mt792x_mutex_release(dev);
}

void mt7925_mlo_pm_work(struct work_struct *work)
{
	struct mt792x_dev *dev = container_of(work, struct mt792x_dev,
					      mlo_pm_work.work);
	struct ieee80211_hw *hw = mt76_hw(dev);

	ieee80211_iterate_active_interfaces(hw,
					    IEEE80211_IFACE_ITER_RESUME_ALL,
					    mt7925_mlo_pm_iter, dev);
}

void
mt7927_mlo_assoc_change_chanctx(struct ieee80211_hw *hw,
		      struct ieee80211_chanctx_conf *ctx,
		      u32 changed)
{
	struct mt792x_chanctx *mctx = (struct mt792x_chanctx *)ctx->drv_priv;
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	struct mt792x_bss_conf *mconf;
	struct ieee80211_vif *vif;
	struct mt792x_vif *mvif;

	if (!mctx->bss_conf)
		return;

	mconf = mctx->bss_conf;
	mvif = mconf->vif;
	vif = container_of((void *)mvif, struct ieee80211_vif, drv_priv);

	mt792x_mutex_acquire(phy->dev);
	if (vif->type == NL80211_IFTYPE_MONITOR) {
		mt7925_monitor_arm_sniffer(phy, vif, ctx);
	} else {
		if (ieee80211_vif_is_mld(vif)) {
			unsigned long valid = vif->valid_links;
			u8 i;

			/* The driver's mask may omit a primed resident link. Never
			 * send an unmatched ctx to the last BSS visited: that can
			 * retune the survivor with the incoming link's channel.
			 */
			mconf = NULL;
			for_each_set_bit(i, &valid, IEEE80211_MLD_MAX_NUM_LINKS) {
				struct mt792x_bss_conf *candidate;

				candidate = mt792x_vif_to_link(mvif, i);
				if (candidate && candidate->link_id == i &&
				    candidate->mt76.ctx == ctx) {
					mconf = candidate;
					break;
				}
			}

			if (READ_ONCE(mt76_mlo_diag_trace) ||
			    READ_ONCE(mt7925_mlo_ctl_trace))
				printk(KERN_INFO
				       "MLO_CHANCTX_TRACE: ctx=%p negotiated_links=0x%x driver_links=0x%x selected_link=%d bss_idx=%d ctx_channel=%u changed=0x%x\n",
				       ctx, vif->valid_links, mvif->valid_links,
				       mconf ? (int)mconf->link_id : -1,
				       mconf ? (int)mconf->mt76.idx : -1,
				       ctx->def.chan->hw_value, changed);
		} else {
			mconf = &mvif->bss_conf;
		}

		if (mconf) {
			struct ieee80211_bss_conf *link_conf;

			if (ieee80211_vif_is_mld(vif))
				mt7927_mlo_pairs_invalidate(vif, false);
			link_conf = mt792x_vif_to_bss_conf(vif, mconf->link_id);
			mt7925_mcu_set_chctx(mvif->phy->mt76, &mconf->mt76,
					     link_conf, ctx);

			if (changed & IEEE80211_CHANCTX_CHANGE_PUNCTURING)
				mt7925_mcu_set_eht_pp(mvif->phy->mt76, &mconf->mt76,
						      link_conf, ctx);
		}
	}
	mt76_phy_chandef_set(mvif->phy->mt76, &ctx->def);

	mt792x_mutex_release(phy->dev);
}

void mt7927_mlo_assoc_vif_cfg_changed(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   u64 changed)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	unsigned long valid = ieee80211_vif_is_mld(vif) ?
				      mvif->valid_links : BIT(0);
	struct ieee80211_bss_conf *bss_conf;
	int i;

	if (changed & BSS_CHANGED_ASSOC) {
		mt7925_mlo_completion_assoc(vif, vif->cfg.assoc);
		mt7927_mlo_steering_kick(vif);
	} else if (changed & (BSS_CHANGED_MLD_VALID_LINKS |
			      BSS_CHANGED_MLD_TTLM)) {
		mt7925_mlo_completion_changed(vif);
	}
	mt792x_mutex_acquire(dev);

	if (changed & BSS_CHANGED_ASSOC) {
		mt7925_mcu_sta_update(dev, NULL, vif,
				      &mvif->sta.deflink, true,
				      MT76_STA_INFO_STATE_ASSOC, NULL);
		mt7925_mcu_set_beacon_filter(dev, vif, vif->cfg.assoc);

		if (ieee80211_vif_is_mld(vif))
			mvif->mlo_pm_state = MT792x_MLO_LINK_ASSOC;

		mt7925_mlo_update_rssi_monitor(dev, vif);
	}

	if (changed & BSS_CHANGED_ARP_FILTER) {
		for_each_set_bit(i, &valid, IEEE80211_MLD_MAX_NUM_LINKS) {
			bss_conf = mt792x_vif_to_bss_conf(vif, i);
			mt7925_mcu_update_arp_filter(&dev->mt76, bss_conf);
		}
	}

	if (changed & BSS_CHANGED_PS) {
		if (hweight16(mvif->valid_links) < 2) {
			/* legacy */
			bss_conf = &vif->bss_conf;
			mt7925_mcu_uni_bss_ps(dev, bss_conf);
		} else {
			if (mvif->mlo_pm_state == MT792x_MLO_LINK_ASSOC) {
				mvif->mlo_pm_state = MT792x_MLO_CHANGED_PS_PENDING;
			} else if (mvif->mlo_pm_state == MT792x_MLO_CHANGED_PS) {
				for_each_set_bit(i, &valid, IEEE80211_MLD_MAX_NUM_LINKS) {
					bss_conf = mt792x_vif_to_bss_conf(vif, i);
					mt7925_mcu_uni_bss_ps(dev, bss_conf);
				}
			}
		}
	}

	mt792x_mutex_release(dev);

/* compat: the cfg80211 NAN API arrived in kernel 7.2 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
	if (vif->type == NL80211_IFTYPE_NAN &&
	    changed & BSS_CHANGED_NAN_LOCAL_SCHED) {
		mt7925_nan_local_sched_changed(dev, vif);
	}
#endif
}

int
mt7927_mlo_assoc_change_vif_links(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			u16 old_links, u16 new_links,
			struct ieee80211_bss_conf *old[IEEE80211_MLD_MAX_NUM_LINKS])
{
	struct mt792x_bss_conf *mconfs[IEEE80211_MLD_MAX_NUM_LINKS] = {}, *mconf;
	struct mt792x_link_sta *mlinks[IEEE80211_MLD_MAX_NUM_LINKS] = {}, *mlink;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long add = new_links & ~old_links;
	unsigned long rem = old_links & ~new_links;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	struct ieee80211_bss_conf *link_conf;
	unsigned long bss_added = 0;
	unsigned int link_id;
	int err;

	if (old_links == new_links)
		return 0;

	mt7925_mlo_completion_changed(vif);
	if (old_links && new_links &&
	    is_mt7927(&dev->mt76) &&
	    ieee80211_vif_is_mld(vif) &&
	    vif->type == NL80211_IFTYPE_STATION) {
		err = mt7927_mlo_change_vif_links(hw, vif, old_links, new_links, old);
		mt7925_mlo_completion_note_error(vif, err);
		return err;
	}

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_LINK_ADD_START: old_links=0x%x new_links=0x%x add=0x%lx rem=0x%lx\n",
		      old_links, new_links, add, rem);

	mt792x_mutex_acquire(dev);

	/* Any MLO JOIN reservation must be handed back before the BSSes it
	 * refers to are destroyed, otherwise the firmware keeps it forever
	 * and never grants another one.
	 */
	if (rem && (mt7925_mlo_roc_release & 2))
		mt7925_mlo_roc_release_transition(phy, mvif, old_links,
						  new_links, vif->valid_links);

	for_each_set_bit(link_id, &rem, IEEE80211_MLD_MAX_NUM_LINKS) {
		mconf = mt792x_vif_to_link(mvif, link_id);
		mlink = mt792x_sta_to_link(&mvif->sta, link_id);

		if (!mconf || !mlink)
			continue;

		if (mconf != &mvif->bss_conf) {
			mt792x_mac_link_bss_remove(dev, mconf, mlink);
			devm_kfree(dev->mt76.dev, mconf);
			devm_kfree(dev->mt76.dev, mlink);
		}

		rcu_assign_pointer(mvif->link_conf[link_id], NULL);
		rcu_assign_pointer(mvif->sta.link[link_id], NULL);
	}

	for_each_set_bit(link_id, &add, IEEE80211_MLD_MAX_NUM_LINKS) {
		if (!old_links) {
			mvif->deflink_id = link_id;
			mconf = &mvif->bss_conf;
			mlink = &mvif->sta.deflink;
		} else {
			mconf = devm_kzalloc(dev->mt76.dev, sizeof(*mconf),
					     GFP_KERNEL);
			mlink = devm_kzalloc(dev->mt76.dev, sizeof(*mlink),
					     GFP_KERNEL);
			if (!mconf || !mlink) {
				mt792x_mutex_release(dev);
				return -ENOMEM;
			}
		}

		mconfs[link_id] = mconf;
		mlinks[link_id] = mlink;
		mconf->link_id = link_id;
		mconf->vif = mvif;
		mlink->wcid.link_id = link_id;
		mlink->wcid.link_valid = !!vif->valid_links;
		mlink->wcid.def_wcid = &mvif->sta.deflink.wcid;
	}

	if (hweight16(mvif->valid_links) == 0)
		mt792x_mac_link_bss_remove(dev, &mvif->bss_conf,
					   &mvif->sta.deflink);

	for_each_set_bit(link_id, &add, IEEE80211_MLD_MAX_NUM_LINKS) {
		mconf = mconfs[link_id];
		mlink = mlinks[link_id];
		link_conf = mt792x_vif_to_bss_conf(vif, link_id);

		rcu_assign_pointer(mvif->link_conf[link_id], mconf);
		rcu_assign_pointer(mvif->sta.link[link_id], mlink);

		err = mt7925_mac_link_bss_add(dev, link_conf, mlink);
		if (err < 0)
			goto free;

		/* The firmware BSS now exists for this link; it must be torn
		 * down properly if a later step fails.
		 */
		__set_bit(link_id, &bss_added);

		if (mconf != &mvif->bss_conf) {
			dev_info(dev->mt76.dev,
				 "CVL roc link_id=%u add=0x%lx active=0x%x valid=0x%x defl=%u defidx=%u omac=%u vif_mask=0x%llx omac_mask=0x%llx\n",
				 link_id, add, vif->active_links,
				 vif->valid_links, mvif->deflink_id,
				 mvif->bss_conf.mt76.idx,
				 mvif->bss_conf.mt76.omac_idx,
				 dev->mt76.vif_mask, phy->omac_mask);

			err = mt7925_set_mlo_roc(phy, &mvif->bss_conf,
						 vif->active_links);
			if (err < 0)
				goto free;
		}
	}

	mvif->valid_links = new_links;

	/* Restore the legacy BSS after disabling MLO. */
	if (old_links && !new_links) {
		mt792x_mac_link_bss_remove(dev, &mvif->bss_conf,
					   &mvif->sta.deflink);
		err = mt7925_mac_link_bss_add(dev, &vif->bss_conf,
					      &mvif->sta.deflink);
		if (err < 0) {
			mt792x_mutex_release(dev);
			return err;
		}
	}

	/* Seed mt7927_mlo.negotiated_links/resident_links here too: this
	 * generic (non-MT7927-dispatched) path is exactly what runs for the
	 * very FIRST link ever created for a vif (old_links == 0, before
	 * mt7927_mlo_active_transition() has anything to dispatch to), so
	 * mt7927_mlo_change_vif_links()'s own entry-point backfill never
	 * gets a chance to run for it -- proven live: resident_links stayed
	 * 0 for a link admitted this way even after that backfill was
	 * added, because this generic function, not the MT7927-specific
	 * one, is what actually created its mconf/mlink objects. Only do
	 * this for MT7927 -- other chips don't use mt7927_mlo state at all.
	 */
	if (is_mt7927(&dev->mt76)) {
		unsigned long to_check = new_links & ~mvif->mt7927_mlo.resident_links;
		unsigned int lid;

		mvif->mt7927_mlo.negotiated_links |= new_links;

		for_each_set_bit(lid, &to_check, IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_bss_conf *lconf =
				rcu_dereference_protected(mvif->link_conf[lid],
							  lockdep_is_held(&dev->mt76.mutex));
			struct mt792x_link_sta *lsta =
				rcu_dereference_protected(mvif->sta.link[lid],
							  lockdep_is_held(&dev->mt76.mutex));

			if (lconf && lsta) {
				mvif->mt7927_mlo.resident_links |= BIT(lid);
				if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
					printk(KERN_INFO
					      "MLO_RESIDENT_TRACE: fn=generic_backfill link=%u negotiated_links=0x%x resident_links=0x%x\n",
					      lid, mvif->mt7927_mlo.negotiated_links,
					      mvif->mt7927_mlo.resident_links);
			}
		}
	}

	mt792x_mutex_release(dev);

	return 0;

free:
	for_each_set_bit(link_id, &add, IEEE80211_MLD_MAX_NUM_LINKS) {
		/* Links whose mt7925_mac_link_bss_add() succeeded own a
		 * firmware BSS plus host state (vif_mask/omac_mask/WCID) and
		 * need the full teardown. Links that failed inside
		 * mt7925_mac_link_bss_add() already unwound their own host
		 * state there and must not be torn down again.
		 */
		if (test_bit(link_id, &bss_added))
			mt792x_mac_link_bss_remove(dev, mconfs[link_id],
						   mlinks[link_id]);

		rcu_assign_pointer(mvif->link_conf[link_id], NULL);
		rcu_assign_pointer(mvif->sta.link[link_id], NULL);

		if (mconfs[link_id] != &mvif->bss_conf)
			devm_kfree(dev->mt76.dev, mconfs[link_id]);
		if (mlinks[link_id] != &mvif->sta.deflink)
			devm_kfree(dev->mt76.dev, mlinks[link_id]);
	}

	mt792x_mutex_release(dev);

	return err;
}
