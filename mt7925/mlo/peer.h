/* SPDX-License-Identifier: ISC */
#ifndef __MT7927_MLO_PEER_H
#define __MT7927_MLO_PEER_H

struct sk_buff;
struct ieee80211_vif;
struct ieee80211_sta;
struct mt792x_bss_conf;
struct mt792x_link_sta;
struct mt76_phy;
struct mt76_sta_cmd_info;

int mt7925_mcu_sta_mld_tlv(struct sk_buff *skb, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta,
			 struct mt792x_bss_conf *mconf,
			 struct mt792x_link_sta *mlink,
			 struct mt792x_link_sta *pending);
void mt7925_mcu_sta_eht_mld_tlv(struct sk_buff *skb,
			     struct ieee80211_vif *vif,
			     struct ieee80211_sta *sta);
bool mt7927_mlo_peer_remove_mld(struct mt76_phy *phy,
			       struct mt76_sta_cmd_info *info,
			       struct mt792x_link_sta *mlink);

#endif
