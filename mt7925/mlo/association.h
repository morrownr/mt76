/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7927_MLO_ASSOCIATION_H
#define __MT7927_MLO_ASSOCIATION_H

void mt7925_mlo_quiesce_iter(void *data, u8 *mac,
			    struct ieee80211_vif *vif);
void mt7925_mlo_resume_iter(void *data, u8 *mac,
			   struct ieee80211_vif *vif);

void mt7927_mlo_assoc_link_info_changed(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif,
				       struct ieee80211_bss_conf *info);

/* The callback veneers preserve their mac80211 lock context. Peer-link
 * helpers retain the existing mt76 mutex contract; no nested acquisition.
 */
int mt7927_mlo_assoc_add_interface(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif);
void mt7927_mlo_assoc_remove_interface(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif);
int mt7927_mlo_assoc_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			    struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			    struct ieee80211_key_conf *key);
void mt7927_mlo_assoc_change_chanctx(struct ieee80211_hw *hw,
				      struct ieee80211_chanctx_conf *ctx,
				      u32 changed);
void mt7927_mlo_assoc_vif_cfg_changed(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif, u64 changed);
int mt7927_mlo_assoc_change_vif_links(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    u16 old_links, u16 new_links,
	struct ieee80211_bss_conf *old[IEEE80211_MLD_MAX_NUM_LINKS]);
int mt7927_mlo_assoc_change_sta_links(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_sta *sta,
				    u16 old_links, u16 new_links);
int mt7927_mlo_assoc_assign_vif_chanctx(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *link_conf,
				     struct ieee80211_chanctx_conf *ctx);
void mt7927_mlo_assoc_unassign_vif_chanctx(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif,
					struct ieee80211_bss_conf *link_conf,
					struct ieee80211_chanctx_conf *ctx);

/* Retained generic device dependencies, implemented once in main.c. */
int mt7925_mac_link_sta_assoc_locked(struct mt76_dev *mdev,
				   struct ieee80211_vif *vif,
				   struct ieee80211_link_sta *link_sta);
int mt7925_add_interface_resources(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif);
void mt7927_mlo_assoc_prepare_bss(struct mt792x_dev *dev,
				struct ieee80211_bss_conf *link_conf,
				struct mt792x_bss_conf *mconf);
int mt7925_abort_roc(struct mt792x_phy *phy, struct mt792x_bss_conf *mconf);
int mt7925_set_link_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
		       struct ieee80211_vif *vif, struct ieee80211_sta *sta,
		       struct ieee80211_key_conf *key, int link_id,
		       struct mt792x_link_sta *mlink);
void mt7925_monitor_arm_sniffer(struct mt792x_phy *phy,
			       struct ieee80211_vif *vif,
			       struct ieee80211_chanctx_conf *ctx);

#endif
