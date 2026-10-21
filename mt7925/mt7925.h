/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2023 MediaTek Inc. */

#ifndef __MT7925_H
#define __MT7925_H

#include "../mt792x.h"
#include "regs.h"

#define MT7925_FILTER_FCSFAIL		BIT(2)
#define MT7925_FILTER_CONTROL		BIT(5)
#define MT7925_FILTER_OTHER_BSS	BIT(6)
#define MT7925_FILTER_ENABLE		BIT(31)

#define MT7925_BEACON_RATES_TBL		25

#define MT7925_TX_RING_SIZE		2048
#define MT7925_TX_MCU_RING_SIZE		256
#define MT7925_TX_FWDL_RING_SIZE	128

#define MT7925_RX_RING_SIZE		1536
#define MT7925_RX_MCU_RING_SIZE		512
#define MT7928_RX_MCU_WA_RING_SIZE	512

#define MT7925_EEPROM_SIZE		3584
#define MT7925_TOKEN_SIZE		8192

#define MT7925_EEPROM_BLOCK_SIZE	16

#define MT7925_SKU_RATE_NUM		161
#define MT7925_SKU_MAX_DELTA_IDX	MT7925_SKU_RATE_NUM
#define MT7925_SKU_TABLE_SIZE		(MT7925_SKU_RATE_NUM + 1)

#define MCU_UNI_EVENT_ROC  0x27

#define HIF_TRAFFIC_IDLE 0x2

enum {
	UNI_EVENT_HIF_CTRL_BASIC = 0,
	UNI_EVENT_HIF_CTRL_TAG_NUM
};

struct mt7925_mcu_hif_ctrl_basic_tlv {
	__le16 tag;
	__le16 len;
	u8 cid;
	u8 pad[3];
	u32 status;
	u8 hif_type;
	u8 hif_tx_traffic_status;
	u8 hif_rx_traffic_status;
	u8 hifsuspend;
	u8 rsv[4];
} __packed;

enum {
	UNI_ROC_ACQUIRE,
	UNI_ROC_ABORT,
	UNI_ROC_SUB_LINK = 3,
	UNI_ROC_NUM
};

enum mt7925_roc_req {
	MT7925_ROC_REQ_JOIN,
	MT7925_ROC_REQ_ROC,
	MT7925_ROC_REQ_SUB_LINK,
	MT7925_ROC_REQ_MLSR_AG = 10,
	MT7925_ROC_REQ_MLSR_AA,
	MT7925_ROC_REQ_NUM
};

enum {
	UNI_EVENT_ROC_GRANT = 0,
	UNI_EVENT_ROC_GRANT_SUB_LINK = 4,
	UNI_EVENT_ROC_TAG_NUM
};

struct mt7925_roc_grant_tlv {
	__le16 tag;
	__le16 len;
	u8 bss_idx;
	u8 tokenid;
	u8 status;
	u8 primarychannel;
	u8 rfsco;
	u8 rfband;
	u8 channelwidth;
	u8 centerfreqseg1;
	u8 centerfreqseg2;
	u8 reqtype;
	u8 dbdcband;
	u8 rsv[1];
	__le32 max_interval;
} __packed;

struct mt7925_beacon_loss_tlv {
	__le16 tag;
	__le16 len;
	u8 reason;
	u8 nr_btolink;
	u8 pad[2];
} __packed;

struct mt7925_uni_beacon_loss_event {
	struct {
		u8 bss_idx;
		u8 pad[3];
	} __packed hdr;
	struct mt7925_beacon_loss_tlv beacon_loss;
} __packed;

struct mt7925_uni_rssi_monitor_event {
		__le16 tag;
		__le16 len;
		__le32 rssi;
} __packed;

#define to_rssi(field, rxv)		((FIELD_GET(field, rxv) - 220) / 2)
#define to_rcpi(rssi)			(2 * (rssi) + 220)

enum mt7925_txq_id {
	MT7925_TXQ_BAND0,
	MT7925_TXQ_BAND1,
	MT7925_TXQ_MCU_WM = 15,
	MT7925_TXQ_FWDL,
};

enum mt7925_rxq_id {
	MT7925_RXQ_BAND0 = 2,
	MT7925_RXQ_BAND1,
	MT7925_RXQ_MCU_WM = 0,
	MT7925_RXQ_MCU_WM2, /* for tx done */
};

enum mt7927_txq_id {
	MT7927_TXQ_BAND0 = MT7925_TXQ_BAND0,
	MT7927_TXQ_BAND1 = MT7925_TXQ_BAND1,
	MT7927_TXQ_MCU_WM = MT7925_TXQ_MCU_WM,
	MT7927_TXQ_FWDL = MT7925_TXQ_FWDL,
};

enum mt7927_rxq_id {
	MT7927_RXQ_BAND0 = 4,
	MT7927_RXQ_MCU_WM = 6,
	MT7927_RXQ_DATA2 = 7,
};

enum mt7928_rxq_id {
	MT7928_RXQ_BAND0,
	MT7928_RXQ_BAND1 = 2,
	MT7928_RXQ_MCU_WM = 3,
	MT7928_RXQ_MCU_WM2 = 1, /* for tx done */
};

enum {
	MODE_OPEN = 0,
	MODE_SHARED = 1,
	MODE_WPA = 3,
	MODE_WPA_PSK = 4,
	MODE_WPA_NONE = 5,
	MODE_WPA2 = 6,
	MODE_WPA2_PSK = 7,
	MODE_WPA3_SAE = 11,
};

enum {
	MT7925_CLC_POWER,
	MT7925_CLC_CHAN,
	MT7925_CLC_MAX_NUM,
};

struct mt7925_clc_rule_v2 {
	u32 flag;
	u8 alpha2[2];
	u8 rsv[10];
} __packed;

struct mt7925_clc_rule {
	u8 alpha2[2];
	u8 type[2];
	u8 seg_idx;
	u8 flag; /* UNII4~8 ctrl flag */
	u8 rsv[2];
} __packed;

struct mt7925_clc_segment {
	u8 idx;
	u8 rsv1[3];
	u32 offset;
	u32 len;
	u8 rsv2[4];
} __packed;

struct mt7925_clc_type0 {
	u8 nr_country;
	u8 type;
	u8 nr_seg;
	u8 rsv[7];
} __packed;

struct mt7925_clc_type2 {
	u8 type;
	u8 rsv[9];
} __packed;

struct mt7925_clc {
	__le32 len;
	u8 idx;
	u8 ver;
	union {
		struct mt7925_clc_type0 t0;
		struct mt7925_clc_type2 t2;
	};
	u8 data[];
} __packed;

enum mt7925_eeprom_field {
	MT_EE_CHIP_ID =		0x000,
	MT_EE_VERSION =		0x002,
	MT_EE_MAC_ADDR =	0x004,
	MT_EE_HW_TYPE =		0xa71,
	__MT_EE_MAX =		0x9ff
};

#define MT_EE_HW_TYPE_ENCAP     GENMASK(1, 0)

enum {
	TXPWR_USER,
	TXPWR_EEPROM,
	TXPWR_MAC,
	TXPWR_MAX_NUM,
};

struct mt7925_txpwr {
	s8 cck[4][2];
	s8 ofdm[8][2];
	s8 ht20[8][2];
	s8 ht40[9][2];
	s8 vht20[12][2];
	s8 vht40[12][2];
	s8 vht80[12][2];
	s8 vht160[12][2];
	s8 he26[12][2];
	s8 he52[12][2];
	s8 he106[12][2];
	s8 he242[12][2];
	s8 he484[12][2];
	s8 he996[12][2];
	s8 he996x2[12][2];
	s8 eht26[16][2];
	s8 eht52[16][2];
	s8 eht106[16][2];
	s8 eht242[16][2];
	s8 eht484[16][2];
	s8 eht996[16][2];
	s8 eht996x2[16][2];
	s8 eht996x4[16][2];
	s8 eht26_52[16][2];
	s8 eht26_106[16][2];
	s8 eht484_242[16][2];
	s8 eht996_484[16][2];
	s8 eht996_484_242[16][2];
	s8 eht996x2_484[16][2];
	s8 eht996x3[16][2];
	s8 eht996x3_484[16][2];
};

static inline u8 mt7927_band_idx(enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return 0;
	case NL80211_BAND_5GHZ:
	case NL80211_BAND_6GHZ:
	default:
		return 1;
	}
}

extern const struct ieee80211_ops mt7925_ops;
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

int __mt7925_start(struct mt792x_phy *phy);
int mt7925_register_device(struct mt792x_dev *dev);
void mt7925_unregister_device(struct mt792x_dev *dev);
int mt7925_run_firmware(struct mt792x_dev *dev);
int mt7925_mcu_set_bss_pm(struct mt792x_dev *dev,
			  struct ieee80211_bss_conf *link_conf,
			  bool enable);
int mt7925_mcu_sta_update(struct mt792x_dev *dev,
			  struct ieee80211_link_sta *link_sta,
			  struct ieee80211_vif *vif,
			  struct mt792x_link_sta *mlink,
			  bool enable,
			  enum mt76_sta_info_state state,
			  struct mt792x_link_sta *pending);
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
int mt7925_mcu_set_chan_info(struct mt792x_phy *phy, u16 tag);
int mt7925_mcu_set_tx(struct mt792x_dev *dev, struct ieee80211_bss_conf *bss_conf);
int mt7925_mcu_set_eeprom(struct mt792x_dev *dev);
int mt7925_mcu_get_rx_rate(struct mt792x_phy *phy, struct ieee80211_vif *vif,
			   struct ieee80211_sta *sta, struct rate_info *rate);
int mt7925_mcu_fw_log_2_host(struct mt792x_dev *dev, u8 ctrl);
void mt7925_mcu_rx_event(struct mt792x_dev *dev, struct sk_buff *skb);
int mt7925_mcu_chip_config(struct mt792x_dev *dev, const char *cmd);
int mt7925_mcu_set_rxfilter(struct mt792x_dev *dev, u32 fif,
			    u8 bit_op, u32 bit_map);

int mt7925_mac_init(struct mt792x_dev *dev);
int mt7925_mac_sta_add(struct mt76_dev *mdev, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta);
bool mt7925_mac_wtbl_update(struct mt792x_dev *dev, int idx, u32 mask);
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
int mt7925_mac_sta_event(struct mt76_dev *mdev, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta, enum mt76_sta_event ev);
void mt7925_mac_sta_remove(struct mt76_dev *mdev, struct ieee80211_vif *vif,
			   struct ieee80211_sta *sta);
void mt7925_mac_reset_work(struct work_struct *work);
int mt7925e_tx_prepare_skb(struct mt76_dev *mdev, void *txwi_ptr,
			   enum mt76_txq_id qid, struct mt76_wcid *wcid,
			   struct ieee80211_sta *sta,
			   struct mt76_tx_info *tx_info);

void mt7925_tx_token_put(struct mt792x_dev *dev);
bool mt7925_rx_check(struct mt76_dev *mdev, void *data, int len);
void mt7925_queue_rx_skb(struct mt76_dev *mdev, enum mt76_rxq_id q,
			 struct sk_buff *skb, u32 *info);
void mt7925_stats_work(struct work_struct *work);
void mt7925_set_stream_he_eht_caps(struct mt792x_phy *phy);
int mt7925_init_mlo_caps(struct mt792x_phy *phy);
int mt7925_init_debugfs(struct mt792x_dev *dev);

int mt7925_mcu_set_beacon_filter(struct mt792x_dev *dev,
				 struct ieee80211_vif *vif,
				 bool enable);
int mt7925_mcu_uni_tx_ba(struct mt792x_dev *dev,
			 struct ieee80211_ampdu_params *params,
			 struct ieee80211_vif *vif, bool enable);
int mt7925_mcu_uni_rx_ba(struct mt792x_dev *dev,
			 struct ieee80211_ampdu_params *params,
			 struct ieee80211_vif *vif, bool enable);
void mt7925_mlo_pm_work(struct work_struct *work);
void mt7925_scan_work(struct work_struct *work);
void mt7925_roc_work(struct work_struct *work);
void mt7925_csa_work(struct work_struct *work);
void mt7925_mlo_steer_work(struct work_struct *work);
#define MT7925_MLO_STEER_INTERVAL	(5 * HZ)
void mt7925_mlo_update_rssi_monitor(struct mt792x_dev *dev,
				    struct ieee80211_vif *vif);
extern int mt7925_mlo_link2_rssi_override;
int mt7925_mcu_set_mlo_rssi_monitor(struct mt792x_dev *dev,
				     struct ieee80211_bss_conf *link_conf,
				     bool enable);
int mt7925_mcu_uni_bss_ps(struct mt792x_dev *dev,
			  struct ieee80211_bss_conf *link_conf);
void mt7925_coredump_work(struct work_struct *work);
int mt7925_get_txpwr_info(struct mt792x_dev *dev, u8 band_idx,
			  struct mt7925_txpwr *txpwr);
void mt7925_mac_set_fixed_rate_table(struct mt792x_dev *dev,
				     u8 tbl_idx, u16 rate_idx);
void mt7925_mac_write_txwi(struct mt76_dev *dev, __le32 *txwi,
			   struct sk_buff *skb, struct mt76_wcid *wcid,
			   struct ieee80211_key_conf *key, int pid,
			   enum mt76_txq_id qid, u32 changed);
void mt7925_txwi_free(struct mt792x_dev *dev, struct mt76_txwi_cache *t,
		      struct ieee80211_sta *sta, struct mt76_wcid *wcid,
		      struct list_head *free_list);
int mt7925_mcu_parse_response(struct mt76_dev *mdev, int cmd,
			      struct sk_buff *skb, int seq);

int mt7925e_mac_reset(struct mt792x_dev *dev);
int mt7925e_mcu_init(struct mt792x_dev *dev);
void mt7925e_axidma_cal_cache_init(struct mt792x_dev *dev);
void mt7925_mac_add_txs(struct mt792x_dev *dev, void *data);
void mt7928_mac_add_txs_msg(struct mt792x_dev *dev, void *evt);
void mt7925_set_runtime_pm(struct mt792x_dev *dev);
void mt7925_mcu_set_suspend_iter(void *priv, u8 *mac,
				 struct ieee80211_vif *vif);
void mt7925_connac_mcu_set_suspend_iter(void *priv, u8 *mac,
					struct ieee80211_vif *vif);
void mt7925_set_ipv6_ns_work(struct work_struct *work);
/* compat: the cfg80211 NAN API arrived in kernel 7.2 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
void mt7925_nan_deferred_work(struct work_struct *work);
#endif

int mt7925_mcu_set_sniffer(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			   bool enable);
int mt7925_mcu_config_sniffer(struct mt792x_vif *vif,
			      struct ieee80211_chanctx_conf *ctx);
void mt7925_monitor_reset_iter(void *priv, u8 *mac,
			       struct ieee80211_vif *vif);
void mt7925_monitor_arm_sniffer(struct mt792x_phy *phy,
				struct ieee80211_vif *vif,
				struct ieee80211_chanctx_conf *ctx);
int mt7925_mcu_get_temperature(struct mt792x_phy *phy);

int mt7925_usb_sdio_tx_prepare_skb(struct mt76_dev *mdev, void *txwi_ptr,
				   enum mt76_txq_id qid, struct mt76_wcid *wcid,
				   struct ieee80211_sta *sta,
				   struct mt76_tx_info *tx_info);
void mt7925_usb_sdio_tx_complete_skb(struct mt76_dev *mdev,
				     struct mt76_queue_entry *e);
bool mt7925_usb_sdio_tx_status_data(struct mt76_dev *mdev, u8 *update);

int mt7925_mcu_uni_add_beacon_offload(struct mt792x_dev *dev,
				      struct ieee80211_hw *hw,
				      struct ieee80211_vif *vif,
				      bool enable);
int mt7925_set_tx_sar_pwr(struct ieee80211_hw *hw,
			  const struct cfg80211_sar_specs *sar);

int mt7925_mcu_regval(struct mt792x_dev *dev, u32 regidx, u32 *val, bool set);
int mt7925_mcu_set_clc(struct mt792x_dev *dev, u8 *alpha2,
		       enum environment_cap env_cap);
int mt7925_mcu_set_mlo_roc(struct mt792x_phy *phy, struct mt792x_bss_conf *mconf,
			   u16 sel_links, int duration, u8 token_id);
int mt7925_mcu_set_roc(struct mt792x_phy *phy, struct mt792x_bss_conf *mconf,
		       struct ieee80211_channel *chan, int duration,
		       enum mt7925_roc_req type, u8 token_id);
int mt7925_mcu_set_sublink_roc(struct mt792x_phy *phy, struct mt792x_bss_conf *mconf,
			       struct ieee80211_channel *chan, u8 dbdcband,
			       int duration, u8 token_id);
int mt7925_set_sublink_roc(struct mt792x_phy *phy, struct mt792x_bss_conf *mconf,
			   struct ieee80211_channel *chan, u8 dbdcband);
int mt7925_mcu_abort_mlo_roc(struct mt792x_phy *phy,
			     struct mt792x_bss_conf *mconf, u8 token_id);
int mt7925_mcu_abort_roc(struct mt792x_phy *phy, struct mt792x_bss_conf *mconf,
			 u8 token_id);
void mt7925_roc_abort_sync(struct mt792x_dev *dev);
int mt7925_mcu_fill_message(struct mt76_dev *mdev, struct sk_buff *skb,
			    int cmd, int *wait_seq);
int mt7925_mcu_add_key(struct mt76_dev *dev, struct ieee80211_vif *vif,
		       struct mt76_connac_sta_key_conf *sta_key_conf,
		       struct ieee80211_key_conf *key, int mcu_cmd,
		       struct mt76_wcid *wcid, enum set_key_cmd cmd,
		       struct mt792x_sta *msta);
int mt7925_mcu_set_rts_thresh(struct mt792x_phy *phy, u32 val);
int mt7925_mcu_wtbl_update_hdr_trans(struct mt792x_dev *dev,
				     struct ieee80211_vif *vif,
				     struct mt792x_bss_conf *mconf,
				     struct mt792x_link_sta *mlink);
int mt7925_mcu_wf_rf_pin_ctrl(struct mt792x_phy *phy);

int mt7925_testmode_cmd(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			void *data, int len);
int mt7925_testmode_dump(struct ieee80211_hw *hw, struct sk_buff *msg,
			 struct netlink_callback *cb, void *data, int len);

int mt7925_mcu_set_rssimonitor(struct mt792x_dev *dev, struct ieee80211_vif *vif);

#include "mlo/association.h"
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
#endif
