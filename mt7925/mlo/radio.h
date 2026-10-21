/* SPDX-License-Identifier: ISC */
#ifndef __MT7927_MLO_RADIO_H
#define __MT7927_MLO_RADIO_H

struct mt792x_phy;
struct mt792x_dev;
struct mt792x_vif;
struct mt792x_bss_conf;
struct ieee80211_vif;
struct ieee80211_channel;
struct mt7925_roc_grant_tlv;

/* Resolved execution inputs; pointers borrow the caller's existing lifetime.
 * This is a request plan, never a second reservation/grant state store.
 */
struct mt7927_mlo_radio_plan {
	struct {
		struct mt792x_bss_conf *mconf;
		struct ieee80211_channel *chan;
		u16 tag;
		u8 id, dbdcband;
	} link[3];
	u8 count, anchor_link, reqtype;
};

/* The partner link's per-link context is installed in the firmware view: its own
 * BSS (not the anchor fallback), registered in the vif mask, band assigned. The
 * ROC request builder refuses a link without it, and the association-time JOIN
 * is deferred while it is missing.
 */
bool mt7927_mlo_link_ctx_installed(struct mt792x_dev *dev,
				   struct mt792x_vif *mvif, unsigned int link_id);

int mt7927_mlo_radio_plan(struct mt792x_phy *phy,
			   struct mt792x_bss_conf *mconf, u16 sel_links,
			   struct mt7927_mlo_radio_plan *plan);
u8 mt7927_mlo_join_roc_dbdcband(struct mt792x_bss_conf *mconf,
			      struct ieee80211_channel *chan, u16 tag,
			      bool is_AG_band, u8 link_id, u8 positional_band);
u8 mt7927_mlo_radio_reqtype(struct mt792x_phy *phy, bool is_AG_band,
			   bool delta);
int mt7927_mlo_radio_test_reqtype(void);
u8 mt7927_mlo_radio_slot_dbdc(struct mt792x_phy *phy,
			     struct mt792x_bss_conf *mconf,
			     struct ieee80211_channel *chan, u8 type,
			     bool is_AG_band, u16 tag, u8 ordinal, bool delta);
bool mt7927_mlo_radio_event(struct mt792x_dev *dev,
			    struct mt7925_roc_grant_tlv *grant, bool sub_link);
bool mt7927_mlo_radio_expire(struct mt792x_phy *phy,
			     struct ieee80211_vif *vif);
void mt7927_mlo_radio_beacon_loss(struct ieee80211_vif *vif, u8 bss_idx);
int mt7927_mlo_radio_begin_offchannel(struct mt792x_phy *phy,
				      struct mt792x_bss_conf *mconf);
int mt7925_abort_roc_token(struct mt792x_phy *phy,
			 struct mt792x_bss_conf *mconf, u8 token_id,
			 bool mlo_join);

#endif
