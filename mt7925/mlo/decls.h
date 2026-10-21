/* SPDX-License-Identifier: BSD-3-Clause-Clear */

#ifndef __MT7925_MLO_DECLS_H
#define __MT7925_MLO_DECLS_H

extern bool mt7925_mt7927_dbdc;
#define MT7925_MLO_SIMUL_LINKS_AUTO	0xff

extern u8 mt7925_mlo_max_simul_links;
extern u8 mt7925_mlo_roc_release;
extern bool mt7925_mlo_ctl_trace;
extern bool mt7925_mlo_sync_peer_phy;
/* Exposed narrowly (was static) so mt7925_set_key()'s SET_KEY trigger for
 * the postkey experiment can check them without duplicating a second copy.
 */
extern bool mt7925_test_mt7927_sublink_roc;
extern bool mt7925_test_mt7927_mlo_roc_attach;
extern bool mt7925_test_mt7927_mlo_join3;
extern bool mt7925_mlo_same_token_extend;
int mt7925_mcu_set_mlo_single_acquire(struct mt792x_phy *phy,
				      struct mt792x_bss_conf *mconf,
				      u16 sel_links, int duration, u8 token_id,
				      int dbdcband);
int mt7925_mcu_set_mlo_attach_roc(struct mt792x_phy *phy,
				 struct mt792x_bss_conf *mconf, u16 sel_links,
				 int duration, u8 token_id);
extern bool mt7925_test_mt7927_ptk_after_mld;
extern bool mt7925_test_mt7927_sublink_postkey;
extern bool mt7925_test_force_str_no_eml;
extern bool mt7925_test_refresh_survivor_mld;

int mt7925_mcu_sta_rebind_phy(struct mt792x_dev *dev,
			      struct ieee80211_link_sta *link_sta,
			      struct ieee80211_vif *vif,
			      struct mt792x_link_sta *mlink);
int mt7925_mcu_mld_presetup(struct mt792x_dev *dev,
			  struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta, unsigned int incoming);
bool mt7925_defer_primary_authorize_active(void);
int mt7925_mcu_sta_authorize(struct mt792x_dev *dev,
			     struct ieee80211_link_sta *link_sta,
			     struct ieee80211_vif *vif,
			     struct mt792x_link_sta *mlink);
int mt7925_mcu_add_bss_info(struct mt792x_phy *phy,
			    struct ieee80211_chanctx_conf *ctx,
			    struct ieee80211_bss_conf *link_conf,
			    struct ieee80211_link_sta *link_sta,
			    int enable);

int mt7925_mac_link_bss_add(struct mt792x_dev *dev,
			    struct ieee80211_bss_conf *link_conf,
			    struct mt792x_link_sta *mlink);
int mt7925_mac_sta_add_links(struct mt792x_dev *dev,
			     struct ieee80211_vif *vif,
			     struct ieee80211_sta *sta,
			     unsigned long new_links);
int mt7925_mac_sta_remove_links(struct mt792x_dev *dev,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta,
				unsigned long old_links);
/* Exposed narrowly (was static) so the postkey grant path's prepare/
 * rollback split in mt7927_mlo.c can undo a partially-created peer link
 * with the REAL firmware removal (sends MT76_STA_INFO_STATE_NONE) instead
 * of only the host-side mt7925_mac_sta_unwind_links_host() cleanup, which
 * never touches firmware STA_REC state at all.
 */
void mt7925_mac_link_sta_remove(struct mt76_dev *mdev,
				struct ieee80211_vif *vif,
				struct ieee80211_link_sta *link_sta,
				struct mt792x_link_sta *mlink);
int mt7925_mlo_reconcile_link(struct mt792x_dev *dev,
			      struct ieee80211_vif *vif,
			      struct ieee80211_sta *sta,
			      struct mt792x_link_sta *mlink,
			      unsigned int link_id);
struct mt7925_group_key_rebind_result {
	unsigned int keys_seen;
	unsigned int gtk_seen;
	unsigned int igtk_seen;
	unsigned int bigtk_seen;
	unsigned int already_current;
	unsigned int installs_attempted;
	unsigned int installs_succeeded;
};
/* Exposed narrowly (was static) so the pre-ROC prekey experiment in
 * mt7927_mlo.c can rebind the substitute's BSS/BMC group keys using only
 * mconf/BSS/BMC-wcid state and mac80211's own cached keys -- no peer
 * STA_REC required, safe to call while the peer's wcid.link_valid is still
 * false. `result` may be NULL; the return value is the first real MCU
 * failure encountered (0 on full success, including an all-already-current
 * no-op run).
 */
int mt7925_mlo_ptk_rebind(struct mt792x_dev *dev,
			  struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta,
			  struct mt792x_link_sta *mlink);
int mt7925_mlo_group_key_rebind(struct mt792x_dev *dev,
				struct ieee80211_vif *vif,
				unsigned int link_id,
				struct mt7925_group_key_rebind_result *result);
int mt7925_mlo_request_active_links(struct mt792x_dev *dev,
				    struct ieee80211_vif *vif,
				    u16 links);
int mt7925_mlo_request_active_links_negotiated(struct mt792x_dev *dev,
					       struct ieee80211_vif *vif,
					       u16 links);
int mt7925_mac_link_sta_assoc(struct mt792x_dev *dev,
			       struct ieee80211_vif *vif,
			       struct ieee80211_link_sta *link_sta);
void mt7925_mlo_join_defer_drop(struct mt792x_vif *mvif, const char *why);
void mt7925_mlo_activate_defer_drop(struct mt792x_vif *mvif, const char *why);
int mt7925_mlo_activate_resume(struct mt792x_dev *dev, struct ieee80211_vif *vif);
int mt7925_set_mlo_roc(struct mt792x_phy *phy,
		       struct mt792x_bss_conf *mconf,
		       u16 sel_links);
int mt7925_mlo_roc_delta_acquire(struct mt792x_phy *phy,
				 struct mt792x_bss_conf *sub_mconf,
				 u16 sel_links, unsigned int exp_slot,
				 unsigned int gen);
void mt7925_mlo_roc_survivor_release(struct mt792x_phy *phy,
				     struct mt792x_vif *mvif,
				     unsigned int survivor_link,
				     unsigned int *old_token,
				     unsigned int *held_before,
				     unsigned int *held_after);
void mt7925_mlo_roc_release_transition(struct mt792x_phy *phy,
				       struct mt792x_vif *mvif,
				       u16 old_links, u16 new_links,
				       u16 valid_links);
void mt7925_abort_mlo_roc_all(struct mt792x_phy *phy,
			      struct mt792x_vif *mvif);
bool mt7925_mlo_roc_abort_guard(struct mt792x_phy *phy,
				struct mt792x_vif *mvif, unsigned int token,
				unsigned int link_id, unsigned int bss_idx,
				const char *site);
bool mt7927_mlo_destructive_test_gate(void);
void mt7925_mlo_roc_release_obsolete(struct mt792x_phy *phy,
				     struct mt792x_vif *mvif);
int mt7927_mlo_change_vif_links(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				u16 old_links, u16 new_links,
				struct ieee80211_bss_conf *old[IEEE80211_MLD_MAX_NUM_LINKS]);
int mt7927_mlo_change_sta_links(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta,
				u16 old_links, u16 new_links);
/* STA-owned two-stage post-key ROC trigger (test_mt7927_sublink_postkey).
 * Stage 1 runs as a wiphy_work, guaranteed by cfg80211's single per-wiphy
 * work pump to execute only after the currently-running wiphy work
 * (ieee80211_activate_links_work(), the whole key_switch_links() loop) has
 * fully returned; it only queues stage 2 and returns. Stage 2 is an
 * ordinary work_struct that acquires dev->mt76.mutex and runs the real
 * postkey attempt. Both are embedded in struct mt792x_sta (not the VIF --
 * a VIF-only worker cannot safely rediscover the peer STA) and initialized
 * once in mt7925_mac_sta_add().
 */
void mt7927_mlo_postkey_stage1_work(struct wiphy *wiphy,
				    struct wiphy_work *work);
void mt7927_mlo_postkey_stage2_work(struct work_struct *work);
/* Single reset point for the postkey pending-operation state on a peer
 * struct mt792x_sta -- shared by the VIF completion phase (on the newly
 * pending record before overwriting a stale one), postkey_attempt_locked()
 * (every terminal exit path), link retirement, and MT76_STA_EVENT_DISASSOC
 * teardown.
 */
void mt7927_mlo_pending_clear(struct mt792x_sta *msta);
int mt7927_mlo_prime_dormant_link(struct mt792x_dev *dev,
				  struct ieee80211_vif *vif,
				  struct mt792x_vif *mvif,
				  unsigned int link_id);

void mt7925_mlo_steer_work(struct work_struct *work);
#define MT7925_MLO_STEER_INTERVAL	(5 * HZ)
void mt7925_mlo_update_rssi_monitor(struct mt792x_dev *dev,
				    struct ieee80211_vif *vif);
extern int mt7925_mlo_link2_rssi_override;
int mt7925_mcu_set_mlo_rssi_monitor(struct mt792x_dev *dev,
				     struct ieee80211_bss_conf *link_conf,
				     bool enable);

int mt7925_mcu_set_sublink_roc(struct mt792x_phy *phy, struct mt792x_bss_conf *mconf,
			       struct ieee80211_channel *chan, u8 dbdcband,
			       int duration, u8 token_id);
int mt7925_set_sublink_roc(struct mt792x_phy *phy, struct mt792x_bss_conf *mconf,
			   struct ieee80211_channel *chan, u8 dbdcband);
int mt7925_mcu_abort_mlo_roc(struct mt792x_phy *phy,
			     struct mt792x_bss_conf *mconf, u8 token_id);

#include "association.h"
struct mt76_wcid *mt7925_mlo_tx_select_wcid(struct mt76_dev *dev,
		struct mt76_wcid *wcid, struct ieee80211_sta *sta,
		struct sk_buff *skb);
struct mt76_wcid *mt7925_mlo_tx_select_link(struct mt76_dev *dev,
		struct ieee80211_vif *vif, struct ieee80211_sta *sta,
		struct sk_buff *skb, struct mt76_wcid *original_wcid);
void mt7925_mlo_tx_update_load(struct mt76_dev *dev);
bool mt7925_mlo_tx_preserve_addr3(struct mt76_dev *dev,
		struct ieee80211_vif *vif, struct sk_buff *skb);
void mt7925_mac_ampdu_link(struct mt792x_dev *dev, struct mt76_wcid *wcid,
		struct ieee80211_ampdu_params *params);
int mt7925_mac_ampdu_action(struct mt792x_dev *dev,
		struct ieee80211_vif *vif, struct ieee80211_ampdu_params *params,
		struct mt76_txq *mtxq);
int mt7925_mlo_tx_ampdu_action(struct ieee80211_hw *hw,
		struct ieee80211_vif *vif, struct ieee80211_ampdu_params *params);

#endif /* __MT7925_MLO_DECLS_H */
