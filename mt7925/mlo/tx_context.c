// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "../mt7925.h"
#include "tx_context.h"

/* Keep experimental address/PN separation off until protected OTA traffic
 * and the original TCP connection have validated the firmware combination.
 */
static bool mt7925e_mlo_tx_pn_owner_live_link;
module_param_named(mlo_tx_pn_owner_live_link,
		   mt7925e_mlo_tx_pn_owner_live_link, bool, 0644);
MODULE_PARM_DESC(mlo_tx_pn_owner_live_link,
		 "EXPERIMENTAL: native MT7927 data uses registered association PN owner and selected live link addresses (default N)");


/* Caller holds RCU through TX status registration. No key or PN is read or
 * rewritten here. The embedded original peer must still be registered:
 * deflink_id is mutable after removal and is NOT a PN-owner identifier.
 */
int
mt7925e_mlo_tx_pn_context(struct mt76_dev *mdev, struct ieee80211_sta *sta,
			struct sk_buff *skb, struct mt76_wcid *live,
			int qid, struct mt7925e_mlo_tx_context *ctx)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_key_conf *key = info->control.hw_key;
	struct ieee80211_hdr *hdr = (void *)skb->data;
	struct mt792x_bss_conf *mconf;
	struct ieee80211_bss_conf *conf;
	struct mt792x_link_sta *mlink;
	struct mt792x_sta *msta;
	struct ieee80211_vif *vif;
	struct mt76_wcid *owner;

	if (!READ_ONCE(mt7925e_mlo_tx_pn_owner_live_link) ||
	    !is_mt7927(mdev) || !mt76_is_mmio(mdev) || !sta || sta->tdls ||
	    qid >= MT_TXQ_PSD ||
	    (info->flags & (IEEE80211_TX_CTL_HW_80211_ENCAP |
			   IEEE80211_TX_CTL_INJECTED)) ||
	    (info->control.flags & IEEE80211_TX_CTRL_PORT_CTRL_PROTO) ||
	    skb->protocol == cpu_to_be16(ETH_P_PAE) ||
	    !key || !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE) ||
	    key->cipher != WLAN_CIPHER_SUITE_CCMP ||
	    skb->len < sizeof(struct ieee80211_hdr_3addr) ||
	    !ieee80211_is_data_present(hdr->frame_control) ||
	    !ieee80211_has_protected(hdr->frame_control) ||
	    !ieee80211_has_tods(hdr->frame_control) ||
	    ieee80211_has_fromds(hdr->frame_control) ||
	    is_multicast_ether_addr(hdr->addr1))
		return 0;

	msta = (struct mt792x_sta *)sta->drv_priv;
	if (!msta->vif)
		return 0;
	vif = container_of((void *)msta->vif, struct ieee80211_vif, drv_priv);
	if (!ieee80211_vif_is_mld(vif) ||
	    vif->type != NL80211_IFTYPE_STATION)
		return 0;

	owner = &msta->deflink.wcid;
	if (owner == live)
		return 0;
	if (owner->idx >= ARRAY_SIZE(mdev->wcid) ||
	    rcu_dereference(mdev->wcid[owner->idx]) != owner ||
	    !owner->sta || msta->deflink.sta != msta ||
	    live->idx >= ARRAY_SIZE(mdev->wcid) ||
	    rcu_dereference(mdev->wcid[live->idx]) != live ||
	    !live->sta || !live->link_valid || live->link_id >= 15 ||
	    !(READ_ONCE(vif->active_links) & BIT(live->link_id)))
		return -ENOLINK;
	if (owner->hw_key_idx != key->keyidx || live->hw_key_idx != key->keyidx)
		return -ENOKEY;
	if (skb_header_cloned(skb) || skb_shared(skb))
		return -EOPNOTSUPP;

	mlink = rcu_dereference(msta->link[live->link_id]);
	mconf = rcu_dereference(msta->vif->link_conf[live->link_id]);
	conf = rcu_dereference(vif->link_conf[live->link_id]);
	if (!mlink || &mlink->wcid != live || mlink->sta != msta ||
	    !mconf || !conf || !conf->bssid || mconf->mt76.band_idx > 1)
		return -ENOLINK;

	ctx->owner = owner;
	ctx->omac = mconf->mt76.omac_idx;
	ctx->wmm = mconf->mt76.wmm_idx;
	ctx->band = mconf->mt76.band_idx;
	ether_addr_copy(ctx->ra, conf->bssid);
	ether_addr_copy(ctx->ta, conf->addr);
	return 1;
}
EXPORT_SYMBOL_GPL(mt7925e_mlo_tx_pn_context);
