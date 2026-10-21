// SPDX-License-Identifier: ISC
/* MLD membership, primary/setup ownership and presetup execution.
 * Existing mt76/mac80211 objects and generic TLV/MCU primitives are reused;
 * these decisions do not create a peer/key store or certify firmware state.
 */
#include <linux/moduleparam.h>
#include "../mt7925.h"
#include "../mcu.h"
#include "../mlo_pair_transition.h"
#include "peer.h"

/* compat: kernel 7.2 renamed the EML delay masks (EMLSR_ -> EML_);
 * the values are unchanged.
 */
#ifndef IEEE80211_EML_CAP_EML_PADDING_DELAY
#define IEEE80211_EML_CAP_EML_PADDING_DELAY	IEEE80211_EML_CAP_EMLSR_PADDING_DELAY
#define IEEE80211_EML_CAP_EML_TRANSITION_DELAY	IEEE80211_EML_CAP_EMLSR_TRANSITION_DELAY
#endif

/* A/B diagnostic: primary ownership and the WCID being set up are separate
 * fields in STA_REC_MLD. Keep the existing association payload by default.
 */
static bool mt7925_mlo_test_setup_wlan_current;
module_param_named(mlo_test_setup_wlan_current,
		   mt7925_mlo_test_setup_wlan_current, bool, 0644);
MODULE_PARM_DESC(mlo_test_setup_wlan_current,
		"TEST-ONLY MT7927 STA_REC_MLD setup WCID follows the updated peer (default off)");

static bool mt7925_test_mld_presetup;
module_param_named(test_mt7927_mld_presetup, mt7925_test_mld_presetup,
		   bool, 0644);
MODULE_PARM_DESC(test_mt7927_mld_presetup,
		"TEST-ONLY MT7927 standalone MLD setup before incoming key reconciliation (default off)");

void
mt7925_mcu_sta_eht_mld_tlv(struct sk_buff *skb,
			   struct ieee80211_vif *vif, struct ieee80211_sta *sta)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct wiphy *wiphy = mvif->phy->mt76->hw->wiphy;
	const struct wiphy_iftype_ext_capab *ext_capa;
	struct sta_rec_eht_mld *eht_mld;
	struct tlv *tlv;
	u16 eml_cap;

	if (!ieee80211_vif_is_mld(vif))
		return;

	tlv = mt76_connac_mcu_add_tlv(skb, STA_REC_EHT_MLD, sizeof(*eht_mld));
	eht_mld = (struct sta_rec_eht_mld *)tlv;
	eht_mld->mld_type = 0xff;

	ext_capa = cfg80211_get_iftype_ext_capa(wiphy,
						ieee80211_vif_type_p2p(vif));
	if (!ext_capa)
		return;

	eml_cap = (vif->cfg.eml_cap & (IEEE80211_EML_CAP_EMLSR_SUPP |
				       IEEE80211_EML_CAP_TRANSITION_TIMEOUT)) |
		  (ext_capa->eml_capabilities & (IEEE80211_EML_CAP_EML_PADDING_DELAY |
						IEEE80211_EML_CAP_EML_TRANSITION_DELAY));

	/*
	 * STR and EMLSR capability are independent per spec: a device can
	 * support both. This chip has proven genuine simultaneous dual-link
	 * TX/RX (STR) repeatedly under test, so always advertise STR support
	 * regardless of whether firmware also reports EMLSR support - the
	 * previous either/or logic silently dropped STR advertisement
	 * whenever EMLSR_SUPP was set, leaving the AP believing this is an
	 * EMLSR-only device that must be told via EML Operating Mode
	 * Notification which single link it is currently listening on. With
	 * no such notification ever sent on local active-link changes, the
	 * AP kept addressing downlink traffic to whatever link it last
	 * believed was active, ignoring genuinely active secondary links.
	 */
	eht_mld->str_cap[0] = BIT(1);
	mvif->mlo_adv_is_str = true;

	if (eml_cap & IEEE80211_EML_CAP_EMLSR_SUPP) {
		eht_mld->eml_cap[0] = u16_get_bits(eml_cap, GENMASK(7, 0));
		eht_mld->eml_cap[1] = u16_get_bits(eml_cap, GENMASK(15, 8));
	}

	memcpy(mvif->mlo_adv_str_cap, eht_mld->str_cap, sizeof(mvif->mlo_adv_str_cap));
	memcpy(mvif->mlo_adv_eml_cap, eht_mld->eml_cap, sizeof(mvif->mlo_adv_eml_cap));
}

int
mt7925_mcu_sta_mld_tlv(struct sk_buff *skb,
		       struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta,
		       struct mt792x_bss_conf *mconf,
		       struct mt792x_link_sta *mlink,
		       struct mt792x_link_sta *pending)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	struct mt792x_dev *dev = mvif->phy->dev;
	struct mt792x_bss_conf *mconf_pri;
	struct mt792x_link_sta *mlink_pri;
	struct sta_rec_mld *mld;
	unsigned long active_links = vif->active_links;
	unsigned int link_id;
	struct tlv *tlv;
	u8 cnt = 0;

	if (!ieee80211_vif_is_mld(vif))
		return 0;
	if (!mconf || !mlink || mconf->link_id != mlink->wcid.link_id)
		return -EINVAL;

	/* mac80211 publishes the new active set after the add callback. */
	active_links |= BIT(mlink->wcid.link_id);
	/* The command subject can be the stable, retired host deflink. During
	 * add, describe the survivor plus the initialized pending peer instead
	 * of adding the subject back to the active firmware topology.
	 */
	if (pending) {
		active_links = vif->active_links | BIT(pending->wcid.link_id);
		/* Initial association may not have published any active links yet.
		 * Its live primary is still a member; a retired subject is not.
		 */
		if (!vif->active_links && mlink->wcid.link_valid)
			active_links |= BIT(mlink->wcid.link_id);
	}
	/* During a post-regrant security rebind the record must describe the
	 * post-grant pair, not the still-unpublished mac80211 active set.
	 */
	active_links = mt7925_mlo_pt_mld_links(active_links,
					       mvif->mt7927_mlo.rebind_target_links);

	/* Validate the objects backing the requested mask before appending a
	 * tag. Silently dropping an absent/misidentified member (or truncating a
	 * third member) lets the caller authorize an incomplete MLD topology.
	 * This checks the existing two-slot encoding; it does not establish a
	 * firmware limit or change primary/setup ownership semantics. link_valid
	 * is also a TX gate during authorization, not firmware admission proof.
	 */
	if (hweight_long(active_links) > ARRAY_SIZE(mld->link))
		return -E2BIG;
	for_each_set_bit(link_id, &active_links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *link_mconf =
			mt792x_vif_to_link(mvif, link_id);
		struct mt792x_link_sta *link_msta =
			mt792x_sta_to_link(msta, link_id);

		if (pending && pending->wcid.link_id == link_id)
			link_msta = pending;
		if (!link_mconf || link_mconf->link_id != link_id ||
		    !link_msta || link_msta->wcid.link_id != link_id)
			return -EINVAL;
	}

	mlink_pri = (msta->deflink_id != IEEE80211_LINK_UNSPECIFIED) ?
		    mt792x_sta_to_link(msta, msta->deflink_id) : NULL;
	if (pending && pending->wcid.link_id == msta->deflink_id)
		mlink_pri = pending;
	mconf_pri = (msta->deflink_id != IEEE80211_LINK_UNSPECIFIED) ?
		    mt792x_vif_to_link(mvif, msta->deflink_id) : NULL;

	/* The host deflink owns stable storage; it is not necessarily a member
	 * of the current two-link firmware topology. If steering removed it,
	 * choose a live member of the published active pair before consuming
	 * STA_REC_MLD's two link slots.
	 */
	if (active_links && mconf_pri && mlink_pri &&
	    (!(active_links & BIT(mlink_pri->wcid.link_id)) ||
	     !mlink_pri->wcid.link_valid)) {
		mconf_pri = NULL;
		mlink_pri = NULL;

		for_each_set_bit(link_id, &active_links,
				 IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_bss_conf *link_mconf;
			struct mt792x_link_sta *link_msta;

			link_mconf = mt792x_vif_to_link(mvif, link_id);
			link_msta = mt792x_sta_to_link(msta, link_id);
			if (pending && pending->wcid.link_id == link_id)
				link_msta = pending;
			if (!link_mconf || !link_msta ||
			    !link_msta->wcid.link_valid)
				continue;

			mconf_pri = link_mconf;
			mlink_pri = link_msta;
			break;
		}
	}

	/* If caller is operating on deflink, reuse its mconf as primary. */
	if ((!mconf_pri || !mlink_pri) && mlink == &msta->deflink &&
	    (!active_links || active_links & BIT(mlink->wcid.link_id))) {
		mconf_pri = mconf;
		mlink_pri = mlink;
	}

	if (!mconf_pri || !mlink_pri) {
		dev_warn_ratelimited(dev->mt76.dev,
					     "mt7925: MLD_TLV_LINK skip (no primary mconf) sta=%pM\n",
				sta->addr);
		return -EINVAL;
	}

	tlv = mt76_connac_mcu_add_tlv(skb, STA_REC_MLD, sizeof(*mld));
	mld = (struct sta_rec_mld *)tlv;
	memcpy(mld->mac_addr, sta->addr, ETH_ALEN);

	mld->primary_id = cpu_to_le16(mlink_pri->wcid.idx);
	/* Vendor mldStarecUpdateMldId uses the primary as secondary until a
	 * second registered member exists. Never name WCID0 for a single-member
	 * group; the two-member loop below replaces this with its real partner.
	 */
	mld->secondary_id = mld->primary_id;
	mld->wlan_id = cpu_to_le16(mlink_pri->wcid.idx);
	if (is_mt7927(&dev->mt76) &&
	    READ_ONCE(mt7925_mlo_test_setup_wlan_current))
		mld->wlan_id = cpu_to_le16(mlink->wcid.idx);

	/* Always encode primary link first. */
	mld->link[cnt].wlan_id = cpu_to_le16(mlink_pri->wcid.idx);
	mld->link[cnt++].bss_idx = mconf_pri->mt76.idx;

	/* STA_REC_MLD describes the complete active MLD, not just the link whose
	 * STA_REC happened to trigger this update. A primary-link refresh after
	 * steering used to emit link_num=1 here, overwriting the preceding
	 * two-link record and leaving the newly-active WCID valid and keyed in
	 * host state but absent from the firmware MLD (and therefore at zero
	 * airtime). Always rebuild the record from mac80211's published active
	 * pair so updates for either member serialize the same topology.
	 */
	active_links &= ~BIT(mlink_pri->wcid.link_id);
	for_each_set_bit(link_id, &active_links,
			 IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *link_mconf;
		struct mt792x_link_sta *link_msta;

		if (cnt >= ARRAY_SIZE(mld->link))
			break;

		link_mconf = mt792x_vif_to_link(mvif, link_id);
		link_msta = mt792x_sta_to_link(msta, link_id);
		if (pending && pending->wcid.link_id == link_id)
			link_msta = pending;
		if (!link_mconf || !link_msta || !link_msta->wcid.link_valid)
			continue;

		if (cnt == 1)
			mld->secondary_id = cpu_to_le16(link_msta->wcid.idx);
		mld->link[cnt].wlan_id = cpu_to_le16(link_msta->wcid.idx);
		mld->link[cnt++].bss_idx = link_mconf->mt76.idx;
	}

	mld->link_num = cnt;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_FW_STA_MLD: update_link=%u primary=%u secondary=%u wlan=%u link_num=%u l0_wlan=%u l0_bss=%u l1_wlan=%u l1_bss=%u\n",
		       mconf->link_id, le16_to_cpu(mld->primary_id),
		       le16_to_cpu(mld->secondary_id), le16_to_cpu(mld->wlan_id),
		       mld->link_num, le16_to_cpu(mld->link[0].wlan_id),
		       mld->link[0].bss_idx, le16_to_cpu(mld->link[1].wlan_id),
		       mld->link[1].bss_idx);
	return 0;
}

/* Vendor CFG_MLD_INFO_PRESETUP sends tag 0x20 separately to each peer.
 * Diagnostic only: preserve the existing two-link builder and key lifecycle.
 * Caller holds the device mutex; the temporary target override is restored
 * before returning, including allocation and command failures.
 */
int mt7925_mcu_mld_presetup(struct mt792x_dev *dev,
			  struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta, unsigned int incoming)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	struct mt792x_bss_conf *bss[2];
	struct mt792x_link_sta *peer[2];
	unsigned long links, saved;
	unsigned int ids[2], i;
	int ret = 0;

	if (!READ_ONCE(mt7925_test_mld_presetup) ||
	    !is_mt7927(&dev->mt76) || !ieee80211_vif_is_mld(vif))
		return 0;
	if (incoming >= IEEE80211_MLD_MAX_NUM_LINKS)
		return -EINVAL;
	links = vif->active_links | BIT(incoming);
	if (hweight16(links) != 2)
		return -EINVAL;
	ids[0] = __ffs(links & ~BIT(incoming));
	ids[1] = incoming;
	for (i = 0; i < 2; i++) {
		bss[i] = mt792x_vif_to_link(mvif, ids[i]);
		peer[i] = mt792x_sta_to_link(msta, ids[i]);
		if (!bss[i] || !peer[i] || !peer[i]->wcid.link_valid)
			return -EINVAL;
	}

	saved = mvif->mt7927_mlo.rebind_target_links;
	if (saved)
		return -EBUSY;
	mvif->mt7927_mlo.rebind_target_links = links;
	for (i = 0; i < 2; i++) {
		struct sk_buff *skb;

		skb = __mt76_connac_mcu_alloc_sta_req(&dev->mt76, &bss[i]->mt76,
						      &peer[i]->wcid,
						      sizeof(struct sta_req_hdr) +
						      sizeof(struct sta_rec_mld));
		if (IS_ERR(skb)) {
			ret = PTR_ERR(skb);
			break;
		}
		ret = mt7925_mcu_sta_mld_tlv(skb, vif, sta, bss[i], peer[i], NULL);
		if (ret) {
			dev_kfree_skb(skb);
			break;
		}
		if (skb->len != sizeof(struct sta_req_hdr) + sizeof(struct sta_rec_mld)) {
			dev_kfree_skb(skb);
			ret = -EINVAL;
			break;
		}
		if (READ_ONCE(mt7925_mlo_ctl_trace)) {
			pr_info("MLO_MLD_PRESETUP: submit link=%u wcid=%u pair=0x%lx len=%u\n",
				ids[i], peer[i]->wcid.idx, links, skb->len);
			print_hex_dump(KERN_INFO, "MLO_MLD_PRESETUP_RAW: ",
				       DUMP_PREFIX_OFFSET, 16, 1, skb->data, skb->len, false);
		}
		ret = mt76_mcu_skb_send_msg(&dev->mt76, skb,
					  MCU_UNI_CMD(STA_REC_UPDATE), true);
		pr_info("MLO_MLD_PRESETUP: reply link=%u wcid=%u ret=%d\n",
			ids[i], peer[i]->wcid.idx, ret);
		if (ret)
			break;
	}
	mvif->mt7927_mlo.rebind_target_links = saved;
	return ret;
}

/* NEGATIVE RESULT, kept default N for reproducibility: also send STA_REC_MLD_OFF when the removed STA_REC belongs to
 * the association's host deflink although siblings remain. Firmware then really releases the retired WCID (UWTBL
 * zeroed), but it was holding the MLD's key and TX PN context: every uplink frame afterwards fails decryption at the AP
 * ("Rx Decryption errors" == Rx data on the new link, 2026-10-13 runs t5/t7). Attach of the incoming link does not
 * need it (run t9).
 */
static bool mt7925_mlo_retire_mld_off;
module_param_named(mlo_retire_mld_off, mt7925_mlo_retire_mld_off, bool, 0644);
MODULE_PARM_DESC(mlo_retire_mld_off,
		 "EXPERIMENTAL: send STA_REC_MLD_OFF when the retired link is the MT7927 MLD deflink (default N)");

bool mt7927_mlo_peer_remove_mld(struct mt76_phy *phy,
			       struct mt76_sta_cmd_info *info,
			       struct mt792x_link_sta *mlink)
{
	return !info->link_sta || !info->link_sta->sta ||
	       !ieee80211_vif_is_mld(info->vif) ||
	       !((struct mt792x_sta *)info->link_sta->sta->drv_priv)->valid_links ||
	       (READ_ONCE(mt7925_mlo_retire_mld_off) && is_mt7927(phy->dev) &&
		info->wcid->idx == mlink->wcid.idx && mlink->sta &&
		mlink->sta->deflink_id == info->wcid->link_id);
}
