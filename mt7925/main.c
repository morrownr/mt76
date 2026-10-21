// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2023 MediaTek Inc. */

#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <net/ipv6.h>
#include "mlo_retouch_primary.h"
#include "mt7925.h"
#include "mlo_roc_delta.h"
#include "mlo_pair_transition.h"
#include "mlo_roc_state.h"
#include "mlo/mlo.h"
#include "mlo/pairs.h"
#include "mlo/association.h"
#include "mlo/radio.h"
#include "regd.h"
#include "mcu.h"
#include "mac.h"
#include "nan.h"



static void
mt7925_init_he_caps(struct mt792x_phy *phy, enum nl80211_band band,
		    struct ieee80211_sband_iftype_data *data,
		    enum nl80211_iftype iftype)
{
	struct ieee80211_sta_he_cap *he_cap = &data->he_cap;
	struct ieee80211_he_cap_elem *he_cap_elem = &he_cap->he_cap_elem;
	struct ieee80211_he_mcs_nss_supp *he_mcs = &he_cap->he_mcs_nss_supp;
	int i, nss = hweight8(phy->mt76->antenna_mask);
	u16 mcs_map = 0;

	for (i = 0; i < 8; i++) {
		if (i < nss)
			mcs_map |= (IEEE80211_HE_MCS_SUPPORT_0_11 << (i * 2));
		else
			mcs_map |= (IEEE80211_HE_MCS_NOT_SUPPORTED << (i * 2));
	}

	he_cap->has_he = true;

	he_cap_elem->mac_cap_info[0] = IEEE80211_HE_MAC_CAP0_HTC_HE;
	he_cap_elem->mac_cap_info[3] = IEEE80211_HE_MAC_CAP3_OMI_CONTROL |
				       IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_EXT_3;
	he_cap_elem->mac_cap_info[4] = IEEE80211_HE_MAC_CAP4_AMSDU_IN_AMPDU;

	if (band == NL80211_BAND_2GHZ)
		he_cap_elem->phy_cap_info[0] =
			IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G;
	else
		he_cap_elem->phy_cap_info[0] =
			IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G |
			IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G;

	he_cap_elem->phy_cap_info[1] =
		IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD;
	he_cap_elem->phy_cap_info[2] =
		IEEE80211_HE_PHY_CAP2_NDP_4x_LTF_AND_3_2US |
		IEEE80211_HE_PHY_CAP2_STBC_TX_UNDER_80MHZ |
		IEEE80211_HE_PHY_CAP2_STBC_RX_UNDER_80MHZ |
		IEEE80211_HE_PHY_CAP2_UL_MU_FULL_MU_MIMO |
		IEEE80211_HE_PHY_CAP2_UL_MU_PARTIAL_MU_MIMO;

	switch (iftype) {
	case NL80211_IFTYPE_AP:
		he_cap_elem->mac_cap_info[2] |=
			IEEE80211_HE_MAC_CAP2_BSR;
		he_cap_elem->mac_cap_info[4] |=
			IEEE80211_HE_MAC_CAP4_BQR;
		he_cap_elem->mac_cap_info[5] |=
			IEEE80211_HE_MAC_CAP5_OM_CTRL_UL_MU_DATA_DIS_RX;
		he_cap_elem->phy_cap_info[3] |=
			IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_TX_QPSK |
			IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_QPSK;
		he_cap_elem->phy_cap_info[6] |=
			IEEE80211_HE_PHY_CAP6_PARTIAL_BW_EXT_RANGE |
			IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT;
		he_cap_elem->phy_cap_info[9] |=
			IEEE80211_HE_PHY_CAP9_TX_1024_QAM_LESS_THAN_242_TONE_RU |
			IEEE80211_HE_PHY_CAP9_RX_1024_QAM_LESS_THAN_242_TONE_RU;
		break;
	case NL80211_IFTYPE_STATION:
		he_cap_elem->mac_cap_info[1] |=
			IEEE80211_HE_MAC_CAP1_TF_MAC_PAD_DUR_16US;

		if (band == NL80211_BAND_2GHZ)
			he_cap_elem->phy_cap_info[0] |=
				IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_RU_MAPPING_IN_2G;
		else
			he_cap_elem->phy_cap_info[0] |=
				IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_RU_MAPPING_IN_5G;

		he_cap_elem->phy_cap_info[1] |=
			IEEE80211_HE_PHY_CAP1_DEVICE_CLASS_A |
			IEEE80211_HE_PHY_CAP1_HE_LTF_AND_GI_FOR_HE_PPDUS_0_8US;
		he_cap_elem->phy_cap_info[3] |=
			IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_TX_QPSK |
			IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_QPSK;
		he_cap_elem->phy_cap_info[4] |=
			IEEE80211_HE_PHY_CAP4_SU_BEAMFORMEE |
			IEEE80211_HE_PHY_CAP4_BEAMFORMEE_MAX_STS_UNDER_80MHZ_4 |
			IEEE80211_HE_PHY_CAP4_BEAMFORMEE_MAX_STS_ABOVE_80MHZ_4;
		he_cap_elem->phy_cap_info[5] |=
			IEEE80211_HE_PHY_CAP5_NG16_SU_FEEDBACK |
			IEEE80211_HE_PHY_CAP5_NG16_MU_FEEDBACK;
		he_cap_elem->phy_cap_info[6] |=
			IEEE80211_HE_PHY_CAP6_CODEBOOK_SIZE_42_SU |
			IEEE80211_HE_PHY_CAP6_CODEBOOK_SIZE_75_MU |
			IEEE80211_HE_PHY_CAP6_TRIG_CQI_FB |
			IEEE80211_HE_PHY_CAP6_PARTIAL_BW_EXT_RANGE |
			IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT;
		he_cap_elem->phy_cap_info[7] |=
			IEEE80211_HE_PHY_CAP7_POWER_BOOST_FACTOR_SUPP |
			IEEE80211_HE_PHY_CAP7_HE_SU_MU_PPDU_4XLTF_AND_08_US_GI;
		he_cap_elem->phy_cap_info[8] |=
			IEEE80211_HE_PHY_CAP8_20MHZ_IN_40MHZ_HE_PPDU_IN_2G |
			IEEE80211_HE_PHY_CAP8_20MHZ_IN_160MHZ_HE_PPDU |
			IEEE80211_HE_PHY_CAP8_80MHZ_IN_160MHZ_HE_PPDU |
			IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_484;
		he_cap_elem->phy_cap_info[9] |=
			IEEE80211_HE_PHY_CAP9_LONGER_THAN_16_SIGB_OFDM_SYM |
			IEEE80211_HE_PHY_CAP9_NON_TRIGGERED_CQI_FEEDBACK |
			IEEE80211_HE_PHY_CAP9_TX_1024_QAM_LESS_THAN_242_TONE_RU |
			IEEE80211_HE_PHY_CAP9_RX_1024_QAM_LESS_THAN_242_TONE_RU |
			IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_COMP_SIGB |
			IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_NON_COMP_SIGB;
		break;
	default:
		break;
	}

	he_mcs->rx_mcs_80 = cpu_to_le16(mcs_map);
	he_mcs->tx_mcs_80 = cpu_to_le16(mcs_map);
	he_mcs->rx_mcs_160 = cpu_to_le16(mcs_map);
	he_mcs->tx_mcs_160 = cpu_to_le16(mcs_map);

	memset(he_cap->ppe_thres, 0, sizeof(he_cap->ppe_thres));

	if (he_cap_elem->phy_cap_info[6] &
	    IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT) {
		mt76_connac_gen_ppe_thresh(he_cap->ppe_thres, nss, band);
	} else {
		he_cap_elem->phy_cap_info[9] |=
			u8_encode_bits(IEEE80211_HE_PHY_CAP9_NOMINAL_PKT_PADDING_16US,
				       IEEE80211_HE_PHY_CAP9_NOMINAL_PKT_PADDING_MASK);
	}

	if (band == NL80211_BAND_6GHZ) {
		struct ieee80211_supported_band *sband =
			&phy->mt76->sband_5g.sband;
		struct ieee80211_sta_ht_cap *ht_cap = &sband->ht_cap;

		u16 cap = IEEE80211_HE_6GHZ_CAP_TX_ANTPAT_CONS |
			  IEEE80211_HE_6GHZ_CAP_RX_ANTPAT_CONS;

		cap |= u16_encode_bits(ht_cap->ampdu_density,
				       IEEE80211_HE_6GHZ_CAP_MIN_MPDU_START) |
		       u16_encode_bits(IEEE80211_VHT_MAX_AMPDU_1024K,
				       IEEE80211_HE_6GHZ_CAP_MAX_AMPDU_LEN_EXP) |
		       u16_encode_bits(IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454,
				       IEEE80211_HE_6GHZ_CAP_MAX_MPDU_LEN);

		data->he_6ghz_capa.capa = cpu_to_le16(cap);
	}
}

static void
mt7925_init_eht_caps(struct mt792x_phy *phy, enum nl80211_band band,
		     struct ieee80211_sband_iftype_data *data)
{
	struct ieee80211_sta_eht_cap *eht_cap = &data->eht_cap;
	struct ieee80211_eht_cap_elem_fixed *eht_cap_elem = &eht_cap->eht_cap_elem;
	struct ieee80211_eht_mcs_nss_supp *eht_nss = &eht_cap->eht_mcs_nss_supp;
	enum nl80211_chan_width width = phy->mt76->chandef.width;
	int nss = hweight8(phy->mt76->antenna_mask);
	int sts = hweight16(phy->mt76->chainmask);
	u8 val;

	if (!phy->dev->has_eht)
		return;

	eht_cap->has_eht = true;

	eht_cap_elem->mac_cap_info[0] =
		IEEE80211_EHT_MAC_CAP0_EPCS_PRIO_ACCESS |
		IEEE80211_EHT_MAC_CAP0_OM_CONTROL;

	eht_cap_elem->phy_cap_info[0] =
		IEEE80211_EHT_PHY_CAP0_NDP_4_EHT_LFT_32_GI |
		IEEE80211_EHT_PHY_CAP0_SU_BEAMFORMER |
		IEEE80211_EHT_PHY_CAP0_SU_BEAMFORMEE;

	if (band == NL80211_BAND_6GHZ && is_320mhz_supported(&phy->dev->mt76))
		eht_cap_elem->phy_cap_info[0] |=
			IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ;

	val = (sts > 3) ? sts - 1 : 3;

	eht_cap_elem->phy_cap_info[0] |=
		u8_encode_bits(u8_get_bits(val, BIT(0)),
			       IEEE80211_EHT_PHY_CAP0_BEAMFORMEE_SS_80MHZ_MASK);

	eht_cap_elem->phy_cap_info[1] =
		u8_encode_bits(u8_get_bits(val, GENMASK(2, 1)),
			       IEEE80211_EHT_PHY_CAP1_BEAMFORMEE_SS_80MHZ_MASK) |
		u8_encode_bits(val,
			       IEEE80211_EHT_PHY_CAP1_BEAMFORMEE_SS_160MHZ_MASK);

	if (band == NL80211_BAND_6GHZ && is_320mhz_supported(&phy->dev->mt76))
		eht_cap_elem->phy_cap_info[1] |=
			u8_encode_bits(val,
				       IEEE80211_EHT_PHY_CAP1_BEAMFORMEE_SS_320MHZ_MASK);

	eht_cap_elem->phy_cap_info[2] =
		u8_encode_bits(sts - 1, IEEE80211_EHT_PHY_CAP2_SOUNDING_DIM_80MHZ_MASK) |
		u8_encode_bits(sts - 1, IEEE80211_EHT_PHY_CAP2_SOUNDING_DIM_160MHZ_MASK);

	if (band == NL80211_BAND_6GHZ && is_320mhz_supported(&phy->dev->mt76))
		eht_cap_elem->phy_cap_info[2] |=
			u8_encode_bits(sts - 1,
				       IEEE80211_EHT_PHY_CAP2_SOUNDING_DIM_320MHZ_MASK);

	eht_cap_elem->phy_cap_info[3] =
		IEEE80211_EHT_PHY_CAP3_NG_16_SU_FEEDBACK |
		IEEE80211_EHT_PHY_CAP3_NG_16_MU_FEEDBACK |
		IEEE80211_EHT_PHY_CAP3_CODEBOOK_4_2_SU_FDBK |
		IEEE80211_EHT_PHY_CAP3_CODEBOOK_7_5_MU_FDBK |
		IEEE80211_EHT_PHY_CAP3_TRIG_SU_BF_FDBK |
		IEEE80211_EHT_PHY_CAP3_TRIG_MU_BF_PART_BW_FDBK |
		IEEE80211_EHT_PHY_CAP3_TRIG_CQI_FDBK;

	eht_cap_elem->phy_cap_info[4] =
		u8_encode_bits(min_t(int, sts - 1, 2),
			       IEEE80211_EHT_PHY_CAP4_MAX_NC_MASK);

	eht_cap_elem->phy_cap_info[5] =
		IEEE80211_EHT_PHY_CAP5_NON_TRIG_CQI_FEEDBACK |
		u8_encode_bits(IEEE80211_EHT_PHY_CAP5_COMMON_NOMINAL_PKT_PAD_16US,
			       IEEE80211_EHT_PHY_CAP5_COMMON_NOMINAL_PKT_PAD_MASK) |
		u8_encode_bits(u8_get_bits(0x11, GENMASK(1, 0)),
			       IEEE80211_EHT_PHY_CAP5_MAX_NUM_SUPP_EHT_LTF_MASK);

	val = width == NL80211_CHAN_WIDTH_320 ? 0xf :
	      width == NL80211_CHAN_WIDTH_160 ? 0x7 :
	      width == NL80211_CHAN_WIDTH_80 ? 0x3 : 0x1;
	eht_cap_elem->phy_cap_info[6] =
		u8_encode_bits(u8_get_bits(0x11, GENMASK(4, 2)),
			       IEEE80211_EHT_PHY_CAP6_MAX_NUM_SUPP_EHT_LTF_MASK) |
		u8_encode_bits(val, IEEE80211_EHT_PHY_CAP6_MCS15_SUPP_MASK);

	eht_cap_elem->phy_cap_info[7] =
		IEEE80211_EHT_PHY_CAP7_NON_OFDMA_UL_MU_MIMO_80MHZ |
		IEEE80211_EHT_PHY_CAP7_NON_OFDMA_UL_MU_MIMO_160MHZ |
		IEEE80211_EHT_PHY_CAP7_MU_BEAMFORMER_80MHZ |
		IEEE80211_EHT_PHY_CAP7_MU_BEAMFORMER_160MHZ;

	val = u8_encode_bits(nss, IEEE80211_EHT_MCS_NSS_RX) |
	      u8_encode_bits(nss, IEEE80211_EHT_MCS_NSS_TX);

	eht_nss->bw._80.rx_tx_mcs9_max_nss = val;
	eht_nss->bw._80.rx_tx_mcs11_max_nss = val;
	eht_nss->bw._80.rx_tx_mcs13_max_nss = val;
	eht_nss->bw._160.rx_tx_mcs9_max_nss = val;
	eht_nss->bw._160.rx_tx_mcs11_max_nss = val;
	eht_nss->bw._160.rx_tx_mcs13_max_nss = val;
	if (band == NL80211_BAND_6GHZ && is_320mhz_supported(&phy->dev->mt76)) {
		eht_nss->bw._320.rx_tx_mcs9_max_nss = val;
		eht_nss->bw._320.rx_tx_mcs11_max_nss = val;
		eht_nss->bw._320.rx_tx_mcs13_max_nss = val;
	}
}


static void
__mt7925_set_stream_he_eht_caps(struct mt792x_phy *phy,
				struct ieee80211_supported_band *sband,
				enum nl80211_band band)
{
	struct ieee80211_sband_iftype_data *data = phy->iftype[band];
	int i, n = 0;

	for (i = 0; i < NUM_NL80211_IFTYPES; i++) {
		switch (i) {
		case NL80211_IFTYPE_STATION:
		case NL80211_IFTYPE_AP:
			break;
		default:
			continue;
		}

		data[n].types_mask = BIT(i);
		mt7925_init_he_caps(phy, band, &data[n], i);
		mt7925_init_eht_caps(phy, band, &data[n]);

		n++;
	}

	_ieee80211_set_sband_iftype_data(sband, data, n);

	for (i = 0; i < n; i++) {
		const struct ieee80211_sta_eht_cap *eht = &data[i].eht_cap;

		dev_info(phy->dev->mt76.dev,
			 "MLO_EHT_CAP_TRACE: register band=%d idx=%d sband=%p iftd=%p n_iftd=%d types=0x%llx sta=%d dev_has_eht=%d has_he=%d has_eht=%d phy0=0x%02x phy1=0x%02x phy2=0x%02x mcs80=%*ph mcs160=%*ph mcs320=%*ph cap320=%d\n",
			 band, i, sband, &data[i], n,
			 (unsigned long long)data[i].types_mask,
			 !!(data[i].types_mask & BIT(NL80211_IFTYPE_STATION)),
			 phy->dev->has_eht, data[i].he_cap.has_he,
			 eht->has_eht, eht->eht_cap_elem.phy_cap_info[0],
			 eht->eht_cap_elem.phy_cap_info[1],
			 eht->eht_cap_elem.phy_cap_info[2],
			 (int)sizeof(eht->eht_mcs_nss_supp.bw._80),
			 &eht->eht_mcs_nss_supp.bw._80,
			 (int)sizeof(eht->eht_mcs_nss_supp.bw._160),
			 &eht->eht_mcs_nss_supp.bw._160,
			 (int)sizeof(eht->eht_mcs_nss_supp.bw._320),
			 &eht->eht_mcs_nss_supp.bw._320,
			 !!(eht->eht_cap_elem.phy_cap_info[0] &
			    IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ));
	}
}

void mt7925_set_stream_he_eht_caps(struct mt792x_phy *phy)
{
	if (phy->mt76->cap.has_2ghz)
		__mt7925_set_stream_he_eht_caps(phy, &phy->mt76->sband_2g.sband,
						NL80211_BAND_2GHZ);

	if (phy->mt76->cap.has_5ghz)
		__mt7925_set_stream_he_eht_caps(phy, &phy->mt76->sband_5g.sband,
						NL80211_BAND_5GHZ);

	if (phy->mt76->cap.has_6ghz)
		__mt7925_set_stream_he_eht_caps(phy, &phy->mt76->sband_6g.sband,
						NL80211_BAND_6GHZ);
}

int __mt7925_start(struct mt792x_phy *phy)
{
	struct mt76_phy *mphy = phy->mt76;
	int err;

	err = mt7925_mcu_set_channel_domain(mphy);
	if (err)
		return err;

	err = mt7925_mcu_set_rts_thresh(phy, 0x92b);
	if (err)
		return err;

	mt792x_mac_reset_counters(phy);
	set_bit(MT76_STATE_RUNNING, &mphy->state);

	ieee80211_queue_delayed_work(mphy->hw, &mphy->mac_work,
				     MT792x_WATCHDOG_TIME);

	if (phy->chip_cap & MT792x_CHIP_CAP_WF_RF_PIN_CTRL_EVT_EN)
		wiphy_rfkill_start_polling(mphy->hw->wiphy);

	return 0;
}
EXPORT_SYMBOL_GPL(__mt7925_start);

static int mt7925_start(struct ieee80211_hw *hw)
{
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	int err;

	mt792x_mutex_acquire(phy->dev);
	err = __mt7925_start(phy);
	mt792x_mutex_release(phy->dev);

	return err;
}


/* Mutex-free core of mt7925_mac_link_sta_assoc(): callers that already hold
 * dev->mt76.mutex (e.g. mt7925_mac_sta_add_links(), which every real caller
 * invokes with the mutex already acquired) must call this directly instead
 * of mt7925_mac_link_sta_assoc() -- that one acquires the mutex itself and
 * would self-deadlock. This was proven live: the deadlock hung the whole
 * kernel (hung_task detector logged unrelated tasks blocked on the same
 * mutex chain) and forced a hardware watchdog reset.
 */
int mt7925_mac_link_sta_assoc(struct mt792x_dev *dev,
			       struct ieee80211_vif *vif,
			       struct ieee80211_link_sta *link_sta)
{
	struct ieee80211_bss_conf *link_conf;
	struct mt792x_link_sta *mlink;
	struct mt792x_sta *msta;
	int err;

	msta = (struct mt792x_sta *)link_sta->sta->drv_priv;
	mlink = mt792x_sta_to_link(msta, link_sta->link_id);

	/* Real bug, pre-existing: this always resolved link_conf via
	 * msta->deflink_id regardless of which link's link_sta was actually
	 * passed in, so admitting a SECONDARY link pushed BSS_INFO using the
	 * PRIMARY's own bss_conf/ctx (band_idx, chanctx, bss_idx all
	 * belonging to the wrong link) combined with the secondary's real
	 * link_sta. Live testing proved the effect: forcing traffic onto a
	 * secondary link kept showing tx_pkts incrementing but
	 * cum_tx_airtime_us stuck at exactly 0, on every association tried,
	 * regardless of which link was secondary -- because every admission
	 * for a non-deflink link was silently using the wrong BSS context
	 * the whole time. Resolve link_conf from the link actually being
	 * admitted instead.
	 */
	if (ieee80211_vif_is_mld(vif)) {
		link_conf = mt792x_vif_to_bss_conf(vif, link_sta->link_id);
	} else {
		link_conf = mt792x_vif_to_bss_conf(vif, vif->bss_conf.link_id);
	}

	if (vif->type == NL80211_IFTYPE_STATION && !link_sta->sta->tdls) {
		struct mt792x_bss_conf *mconf;

		mconf = mt792x_link_conf_to_mconf(link_conf);
		err = mt7925_mcu_add_bss_info(&dev->phy, mconf->mt76.ctx,
					     link_conf, link_sta, true);
		if (err)
			return err;
	}

	ewma_avg_signal_init(&mlink->avg_ack_signal);

	mt7925_mac_wtbl_update(dev, mlink->wcid.idx,
			       MT_WTBL_UPDATE_ADM_COUNT_CLEAR);
	memset(mlink->airtime_ac, 0, sizeof(mlink->airtime_ac));

	return mt7925_mcu_sta_update(dev, link_sta, vif, mlink, true,
				     MT76_STA_INFO_STATE_ASSOC, NULL);
}

int mt7925_mac_link_sta_assoc_locked(struct mt76_dev *mdev,
					     struct ieee80211_vif *vif,
					     struct ieee80211_link_sta *link_sta)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	int err;

	mt792x_mutex_acquire(dev);
	err = mt7925_mac_link_sta_assoc(dev, vif, link_sta);
	mt792x_mutex_release(dev);
	return err;
}

void mt7925_mac_link_sta_remove(struct mt76_dev *mdev,
				struct ieee80211_vif *vif,
				struct ieee80211_link_sta *link_sta,
				struct mt792x_link_sta *mlink)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	struct mt76_wcid *wcid = &mlink->wcid;
	struct ieee80211_bss_conf *link_conf;
	u8 link_id = link_sta->link_id;
	u16 idx = wcid->idx;

	mt7925_roc_abort_sync(dev);

	mt76_connac_free_pending_tx_skbs(&dev->pm, wcid);
	mt76_connac_pm_wake(&dev->mphy, &dev->pm);

	mt7925_mcu_sta_update(dev, link_sta, vif, mlink, false,
			      MT76_STA_INFO_STATE_NONE, NULL);
	mt7925_mac_wtbl_update(dev, mlink->wcid.idx,
			       MT_WTBL_UPDATE_ADM_COUNT_CLEAR);

	link_conf = mt792x_vif_to_bss_conf(vif, link_id);

	if (vif->type == NL80211_IFTYPE_STATION && !link_sta->sta->tdls) {
		struct mt792x_bss_conf *mconf;

		mconf = mt792x_link_conf_to_mconf(link_conf);

		if (ieee80211_vif_is_mld(vif))
			mt792x_mac_link_bss_remove(dev, mconf, mlink);
		else
			mt7925_mcu_add_bss_info(&dev->phy, mconf->mt76.ctx, link_conf,
						link_sta, false);
	}

	spin_lock_bh(&mdev->sta_poll_lock);
	if (!list_empty(&mlink->wcid.poll_list))
		list_del_init(&mlink->wcid.poll_list);
	spin_unlock_bh(&mdev->sta_poll_lock);

	rcu_assign_pointer(dev->mt76.wcid[idx], NULL);
	mt76_wcid_cleanup(mdev, wcid);
	mt76_wcid_mask_clear(mdev->wcid_mask, idx);

	mt76_connac_power_save_sched(&dev->mphy, &dev->pm);
}

int mt7925_mac_link_bss_add(struct mt792x_dev *dev,
				   struct ieee80211_bss_conf *link_conf,
				   struct mt792x_link_sta *mlink)
{
	struct mt792x_bss_conf *mconf = mt792x_link_conf_to_mconf(link_conf);
	struct ieee80211_vif *vif = link_conf->vif;
	struct mt792x_vif *mvif = mconf->vif;
	struct mt76_txq *mtxq;
	int idx, ret = 0;

	if (vif->type == NL80211_IFTYPE_P2P_DEVICE) {
		mconf->mt76.idx = MT792x_MAX_INTERFACES;
	} else {
		mconf->mt76.idx = __ffs64(~dev->mt76.vif_mask);

		if (mconf->mt76.idx >= MT792x_MAX_INTERFACES) {
			ret = -ENOSPC;
			goto out;
		}
	}

	if (ieee80211_vif_is_mld(vif)) {
		mt7927_mlo_assoc_prepare_bss(dev, link_conf, mconf);
	} else {
		mconf->mt76.omac_idx = mconf->mt76.idx;
		mconf->mt76.band_idx = 0xff;
		if (is_mt7927(&dev->mt76)) {
			struct ieee80211_channel *chan = link_conf->chanreq.oper.chan;

			if (!chan)
				chan = mvif->phy->mt76->chandef.chan;
			mconf->mt76.band_idx = mt7927_band_idx(chan->band);
		}
		mconf->mt76.wmm_idx = mconf->mt76.idx % MT76_CONNAC_MAX_WMM_SETS;
		mconf->mt76.link_idx = hweight16(mvif->valid_links);
	}

	if (mvif->phy->mt76->chandef.chan &&
	    mvif->phy->mt76->chandef.chan->band != NL80211_BAND_2GHZ)
		mconf->mt76.basic_rates_idx = MT792x_BASIC_RATES_TBL + 4;
	else
		mconf->mt76.basic_rates_idx = MT792x_BASIC_RATES_TBL;

	dev->mt76.vif_mask |= BIT_ULL(mconf->mt76.idx);
	mvif->phy->omac_mask |= BIT_ULL(mconf->mt76.omac_idx);

	idx = MT792x_WTBL_RESERVED - mconf->mt76.idx;

	mlink->wcid.idx = idx;
	mlink->wcid.tx_info |= MT_WCID_TX_INFO_SET;
	mt76_wcid_init(&mlink->wcid, 0);

	mt7925_mac_wtbl_update(dev, idx,
			       MT_WTBL_UPDATE_ADM_COUNT_CLEAR);

	ewma_rssi_init(&mconf->rssi);

	rcu_assign_pointer(dev->mt76.wcid[idx], &mlink->wcid);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_BSS_ADD_REACHED: link_id=%u is_primary=%d bss_idx=%u wcid=%u\n",
		      mconf->link_id, mconf == &mvif->bss_conf,
		      mconf->mt76.idx, mlink->wcid.idx);

	ret = mt76_connac_mcu_uni_add_dev(&dev->mphy, link_conf, &mconf->mt76,
					  &mlink->wcid, true);
	if (ret)
		goto free_host;

	if (vif->txq) {
		mtxq = (struct mt76_txq *)vif->txq->drv_priv;
		mtxq->wcid = idx;
	}

	return 0;

free_host:
	/* The firmware BSS was never created, so unwind only the host-side
	 * state allocated above. Do NOT call mt792x_mac_link_bss_remove()
	 * here: it would issue a UNI_ADD_DEV(false) for a device the firmware
	 * does not know about.
	 */
	rcu_assign_pointer(dev->mt76.wcid[idx], NULL);

	dev->mt76.vif_mask &= ~BIT_ULL(mconf->mt76.idx);
	mvif->phy->omac_mask &= ~BIT_ULL(mconf->mt76.omac_idx);

	spin_lock_bh(&dev->mt76.sta_poll_lock);
	if (!list_empty(&mlink->wcid.poll_list))
		list_del_init(&mlink->wcid.poll_list);
	spin_unlock_bh(&dev->mt76.sta_poll_lock);

	mt76_wcid_cleanup(&dev->mt76, &mlink->wcid);

out:
	return ret;
}
/* Caller holds dev->mt76.mutex; MLO lifecycle work stays in the caller. */
int mt7925_add_interface_resources(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	int ret;

	mvif->phy = phy;
	mvif->bss_conf.vif = mvif;
	mvif->sta.vif = mvif;
	mvif->deflink_id = IEEE80211_LINK_UNSPECIFIED;
	ret = mt7925_mac_link_bss_add(dev, &vif->bss_conf, &mvif->sta.deflink);
	if (ret < 0)
		return ret;
	if (vif->type != NL80211_IFTYPE_MONITOR)
		vif->driver_flags |= IEEE80211_VIF_BEACON_FILTER;
	if (phy->chip_cap & MT792x_CHIP_CAP_RSSI_NOTIFY_EVT_EN)
		vif->driver_flags |= IEEE80211_VIF_SUPPORTS_CQM_RSSI;
	INIT_WORK(&mvif->csa_work, mt7925_csa_work);
	timer_setup(&mvif->csa_timer, mt792x_csa_timer, 0);

	if (vif->type == NL80211_IFTYPE_NAN)
		dev->nan_vif = vif;

	return 0;
}

static int
mt7925_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	return mt7927_mlo_assoc_add_interface(hw, vif);
}

static void
mt7925_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	mt7927_mlo_assoc_remove_interface(hw, vif);
}

static void mt7925_roc_iter(void *priv, u8 *mac,
			    struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_phy *phy = priv;

	if (mt7927_mlo_radio_expire(phy, vif))
		return;
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "ROC_ABORT: site=roc_iter_early token=%u mlo_links=0x%x\n",
		       phy->roc_token_id, phy->mlo_roc.pending_links);
	if (mt7925_mlo_roc_abort_guard(phy, mvif, phy->roc_token_id,
				       mvif->bss_conf.link_id,
				       mvif->bss_conf.mt76.idx,
				       "roc_iter_early"))
		mt7925_mcu_abort_roc(phy, &mvif->bss_conf, phy->roc_token_id);
}


void mt7925_roc_abort_sync(struct mt792x_dev *dev)
{
	struct mt792x_phy *phy = &dev->phy;

	if (!test_and_clear_bit(MT76_STATE_ROC, &phy->mt76->state))
		return;

	timer_delete_sync(&phy->roc_timer);

	cancel_work(&phy->roc_work);

	ieee80211_iterate_interfaces(mt76_hw(dev),
				     IEEE80211_IFACE_ITER_RESUME_ALL,
				     mt7925_roc_iter, (void *)phy);
}
EXPORT_SYMBOL_GPL(mt7925_roc_abort_sync);

void mt7925_roc_work(struct work_struct *work)
{
	struct mt792x_phy *phy;

	phy = (struct mt792x_phy *)container_of(work, struct mt792x_phy,
						roc_work);

	if (!test_and_clear_bit(MT76_STATE_ROC, &phy->mt76->state))
		return;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "ROC_WORK_RUN: site=roc_work token=%u mlo_links=0x%x\n",
		       phy->roc_token_id, phy->mlo_roc.pending_links);

	mt792x_mutex_acquire(phy->dev);
	ieee80211_iterate_active_interfaces(phy->mt76->hw,
					    IEEE80211_IFACE_ITER_RESUME_ALL,
					    mt7925_roc_iter, phy);
	mt792x_mutex_release(phy->dev);
	ieee80211_remain_on_channel_expired(phy->mt76->hw);
}


int mt7925_abort_roc(struct mt792x_phy *phy,
			    struct mt792x_bss_conf *mconf)
{
	int err = 0;

	timer_delete_sync(&phy->roc_timer);
	cancel_work_sync(&phy->roc_work);

	mt792x_mutex_acquire(phy->dev);
	if (test_and_clear_bit(MT76_STATE_ROC, &phy->mt76->state)) {
		/* A ROC acquired for a secondary MLO link's bss_conf (e.g.
		 * the JOIN-type ROC mt7925_mgd_prepare_tx() takes on a
		 * non-primary link's chanctx during multi-link auth/assoc)
		 * is granted by firmware under a real dbdcband, not the
		 * "auto" (0xff) that was requested. Releasing it with the
		 * generic 0xff abort silently fails to clear the firmware
		 * side reservation, leaking it across later associations --
		 * same asymmetry mt7925_roc_iter() already works around
		 * above for the explicit MLO JOIN path.
		 */
		err = mt7925_abort_roc_token(phy, mconf,
					     phy->roc_token_id, false);
	}
	mt792x_mutex_release(phy->dev);

	return err;
}

static int mt7925_set_roc(struct mt792x_phy *phy,
			  struct mt792x_bss_conf *mconf,
			  struct ieee80211_channel *chan,
			  int duration,
			  enum mt7925_roc_req type)
{
	int err;

	err = mt7927_mlo_radio_begin_offchannel(phy, mconf);
	if (err)
		return err;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "ROC_REQ_START: site=set_roc token=%u duration=%d\n",
		       phy->roc_token_id + 1, duration);

	err = mt7925_mcu_set_roc(phy, mconf, chan, duration, type,
				 ++phy->roc_token_id);
	if (err < 0) {
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		goto out;
	}

	if (!wait_event_timeout(phy->roc_wait, phy->roc_grant, 4 * HZ)) {
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "ROC_TIMEOUT: site=set_roc token=%u\n",
			       phy->roc_token_id);
		mt7925_abort_roc_token(phy, mconf, phy->roc_token_id, false);
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		err = -ETIMEDOUT;
	}

out:
	return err;
}


static int mt7925_remain_on_channel(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_channel *chan,
				    int duration,
				    enum ieee80211_roc_type type)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	int err;

	mt792x_mutex_acquire(phy->dev);
	err = mt7925_set_roc(phy, &mvif->bss_conf,
			     chan, duration, MT7925_ROC_REQ_ROC);
	mt792x_mutex_release(phy->dev);

	return err;
}

static int mt7925_cancel_remain_on_channel(struct ieee80211_hw *hw,
					   struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_phy *phy = mt792x_hw_phy(hw);

	return mt7925_abort_roc(phy, &mvif->bss_conf);
}

int mt7925_set_link_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			       struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			       struct ieee80211_key_conf *key, int link_id,
			       struct mt792x_link_sta *mlink)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_sta *msta = sta ? (struct mt792x_sta *)sta->drv_priv :
				  &mvif->sta;
	struct ieee80211_bss_conf *link_conf;
	struct ieee80211_link_sta *link_sta;
	int idx = key->keyidx, err = 0;
	struct mt792x_bss_conf *mconf;
	struct mt76_wcid *wcid;
	u8 *wcid_keyidx;

	link_conf = mt792x_vif_to_bss_conf(vif, link_id);
	link_sta = sta ? mt792x_sta_to_link_sta(vif, sta, link_id) : NULL;
	mconf = mt792x_vif_to_link(mvif, link_id);
	wcid = &mlink->wcid;
	wcid_keyidx = &wcid->hw_key_idx;

	/* fall back to sw encryption for unsupported ciphers */
	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_AES_CMAC:
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIE;
		wcid_keyidx = &wcid->hw_key_idx2;
		break;
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104:
		if (!mvif->wep_sta)
			return -EOPNOTSUPP;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
	case WLAN_CIPHER_SUITE_CCMP:
	case WLAN_CIPHER_SUITE_CCMP_256:
	case WLAN_CIPHER_SUITE_GCMP:
	case WLAN_CIPHER_SUITE_GCMP_256:
	case WLAN_CIPHER_SUITE_SMS4:
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (cmd == SET_KEY && !mconf->mt76.cipher) {
		struct mt792x_phy *phy = mt792x_hw_phy(hw);

		mconf->mt76.cipher = mt7925_mcu_get_cipher(key->cipher);
		mt7925_mcu_add_bss_info(phy, mconf->mt76.ctx, link_conf,
					link_sta, true);
	}

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_KEY_KEYIDX_TRACE: entry cmd=%d link_id=%d keyidx=%d cipher=0x%x mlink=%p wcid=%p wcid_idx=%u hw_key_idx_before=%d hw_key_idx2_before=%d field=%s deflink=%p deflink_id=%d msta=%p msta_link_ptr=%p valid_links=0x%x\n",
		      cmd, link_id, idx, key->cipher, mlink, wcid, wcid->idx,
		      wcid->hw_key_idx, wcid->hw_key_idx2,
		      wcid_keyidx == &wcid->hw_key_idx ? "hw_key_idx" : "hw_key_idx2",
		      &msta->deflink, msta->deflink_id, msta,
		      rcu_dereference_protected(msta->link[link_id],
			lockdep_is_held(&dev->mt76.mutex)),
		      msta->valid_links);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_KEY_KEYIDX_TRACE: sta_ptr=%p (%s) mvif=%p mvif_sta_addr=%p\n",
		      sta, sta ? "PAIRWISE/real-sta" : "GROUP/mvif-sta",
		      mvif, &mvif->sta);

	if (cmd == SET_KEY)
		*wcid_keyidx = idx;
	else if (idx == *wcid_keyidx)
		*wcid_keyidx = -1;
	else {
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			      "MLO_KEY_KEYIDX_TRACE: early goto-out (idx=%d != cur=%d), field unchanged\n",
			      idx, *wcid_keyidx);
		goto out;
	}

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_KEY_KEYIDX_TRACE: assigned wcid_idx=%u field=%s value_after=%d\n",
		      wcid->idx, wcid_keyidx == &wcid->hw_key_idx ? "hw_key_idx" : "hw_key_idx2",
		      *wcid_keyidx);

	mt76_wcid_key_setup(&dev->mt76, wcid,
			    cmd == SET_KEY ? key : NULL);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_KEY_KEYIDX_TRACE: after wcid_key_setup wcid_idx=%u hw_key_idx=%d hw_key_idx2=%d rx_check_pn=%d\n",
		      wcid->idx, wcid->hw_key_idx, wcid->hw_key_idx2, wcid->rx_check_pn);

	err = mt7925_mcu_add_key(&dev->mt76, vif, &mlink->bip,
				 key, MCU_UNI_CMD(STA_REC_UPDATE),
				 &mlink->wcid, cmd, msta);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "MLO_KEY_KEYIDX_TRACE: mcu_add_key ret=%d wcid_idx=%u hw_key_idx=%d hw_key_idx2=%d\n",
		      err, wcid->idx, wcid->hw_key_idx, wcid->hw_key_idx2);

	if (err)
		goto out;

	if (key->cipher == WLAN_CIPHER_SUITE_WEP104 ||
	    key->cipher == WLAN_CIPHER_SUITE_WEP40)
		err = mt7925_mcu_add_key(&dev->mt76, vif, &mvif->wep_sta->deflink.bip,
					 key, MCU_WMWA_UNI_CMD(STA_REC_UPDATE),
					 &mvif->wep_sta->deflink.wcid, cmd, msta);
out:
	return err;
}

static int
mt7925_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
		struct ieee80211_vif *vif, struct ieee80211_sta *sta,
		struct ieee80211_key_conf *key)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_sta *msta = sta ? (void *)sta->drv_priv : &mvif->sta;
	struct mt792x_link_sta *mlink;
	int err;

	/* Ordinary key capability checks remain on the generic callback path. */
	if ((vif->type == NL80211_IFTYPE_ADHOC ||
	     vif->type == NL80211_IFTYPE_MESH_POINT) &&
	    (key->cipher == WLAN_CIPHER_SUITE_TKIP ||
	     key->cipher == WLAN_CIPHER_SUITE_CCMP) &&
	    !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE))
		return -EOPNOTSUPP;
	if (ieee80211_vif_is_mld(vif))
		return mt7927_mlo_assoc_set_key(hw, cmd, vif, sta, key);

	mt7925_mlo_completion_changed(vif);
	mt792x_mutex_acquire(dev);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_KEY_INSTALL: cmd=%d cipher=0x%x key_link_id=%d\n",
		       cmd, key->cipher, key->link_id);
	if (cmd == SET_KEY)
		mt76_mlo_bssinfo_deferred_send();
	mlink = mt792x_sta_to_link(msta, vif->bss_conf.link_id);
	err = mt7925_set_link_key(hw, cmd, vif, sta, key,
				  vif->bss_conf.link_id, mlink);
	mt792x_mutex_release(dev);
	mt7925_mlo_completion_note_error(vif, err);
	return err;
}

static void
mt7925_pm_interface_iter(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt792x_dev *dev = priv;
	struct ieee80211_hw *hw = mt76_hw(dev);
	/* Monitor vifs must never have beacon filtering / PM applied. */
	bool pm_enable = dev->pm.enable && vif->type != NL80211_IFTYPE_MONITOR;
	int err;

	err = mt7925_mcu_set_beacon_filter(dev, vif, pm_enable);
	if (err < 0)
		return;

	if (pm_enable) {
		vif->driver_flags |= IEEE80211_VIF_BEACON_FILTER;
		ieee80211_hw_set(hw, CONNECTION_MONITOR);
	} else {
		vif->driver_flags &= ~IEEE80211_VIF_BEACON_FILTER;
		__clear_bit(IEEE80211_HW_CONNECTION_MONITOR, hw->flags);
	}
}

static void
mt7925_monitor_update_chan(struct mt792x_vif *mvif,
			   struct ieee80211_chanctx_conf *ctx)
{
	struct mt792x_bss_conf *mconf = &mvif->bss_conf;
	struct ieee80211_channel *chan;

	if (!is_mt7927(&mvif->phy->dev->mt76))
		return;

	chan = ctx ? ctx->def.chan : mvif->phy->mt76->chandef.chan;
	if (!chan)
		return;

	if (ctx)
		mvif->phy->mt76->chandef = ctx->def;

	mconf->mt76.band_idx = mt7927_band_idx(chan->band);
	mconf->mt76.basic_rates_idx = MT792x_BASIC_RATES_TBL;
	if (chan->band != NL80211_BAND_2GHZ)
		mconf->mt76.basic_rates_idx += 4;
}

/* Re-arm the firmware sniffer for a monitor vif on the channel context @ctx.
 * Shared by the assign-chanctx and change-chanctx paths so monitor mode
 * follows channel/band changes across 2.4/5/6 GHz, not just the band it was
 * first enabled on. Caller must hold dev->mt76.mutex.
 */
void
mt7925_monitor_arm_sniffer(struct mt792x_phy *phy, struct ieee80211_vif *vif,
			   struct ieee80211_chanctx_conf *ctx)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_bss_conf *mconf = &mvif->bss_conf;
	struct mt792x_dev *dev = phy->dev;

	/* On a cross-band move, disable the sniffer on the old physical band
	 * first so the firmware re-arms cleanly on the new one.
	 *
	 * mt7925_mcu_set_sniffer() resolves its band_idx from
	 * mvif->bss_conf.mt76.ctx (falling back to the PHY chandef only when
	 * that pointer is NULL), not from an argument we control. On the
	 * assign_vif_chanctx path the caller may have already pointed
	 * mconf->mt76.ctx at the new ctx before calling in here, and on
	 * change_chanctx @ctx is the same object mac80211 already mutated to
	 * the new channel. Either way, calling mt7925_mcu_set_sniffer()
	 * as-is would report the *new* band on what is supposed to be the
	 * old-band teardown. Temporarily clear the ctx pointer so the lookup
	 * falls back to phy->mt76->chandef.chan, which mt7925_monitor_update_chan()
	 * below hasn't overwritten yet and is therefore still the old band.
	 */
	if (is_mt7927(&dev->mt76) && phy->mt76->chandef.chan && ctx->def.chan &&
	    mt7927_band_idx(phy->mt76->chandef.chan->band) !=
	    mt7927_band_idx(ctx->def.chan->band)) {
		struct ieee80211_chanctx_conf *old_ctx = mconf->mt76.ctx;

		mconf->mt76.ctx = NULL;
		mt7925_mcu_set_sniffer(dev, vif, false);
		mconf->mt76.ctx = old_ctx;
	}

	mt7925_monitor_update_chan(mvif, ctx);
	mconf->mt76.ctx = ctx;
	mt7925_mcu_set_sniffer(dev, vif, true);
	mt7925_mcu_config_sniffer(mvif, ctx);
	/* A sniffer must see other-BSS beacons; never beacon-filter it. */
	mt7925_mcu_set_beacon_filter(dev, vif, false);
}

static void
mt7925_sniffer_interface_iter(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt792x_dev *dev = priv;
	struct ieee80211_hw *hw = mt76_hw(dev);
	struct mt76_connac_pm *pm = &dev->pm;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_chanctx_conf *ctx = mvif->bss_conf.mt76.ctx;
	bool monitor = !!(hw->conf.flags & IEEE80211_CONF_MONITOR);

	if (monitor && !ctx)
		return;

	if (monitor)
		mt7925_monitor_update_chan(mvif, ctx);

	mt7925_mcu_set_sniffer(dev, vif, monitor);
	if (monitor && is_mt7927(&dev->mt76))
		mt7925_mcu_config_sniffer(mvif, ctx);
	pm->enable = pm->enable_user && !monitor;
	pm->ds_enable = pm->ds_enable_user && !monitor;

	mt7925_mcu_set_deep_sleep(dev, pm->ds_enable);

	if (monitor)
		mt7925_mcu_set_beacon_filter(dev, vif, false);
}

/* Re-arm a monitor vif's firmware sniffer after a chip reset. Unlike
 * mt7925_sniffer_interface_iter() above, this does not gate on
 * hw->conf.flags & IEEE80211_CONF_MONITOR: this driver sets
 * NO_VIRTUAL_MONITOR unconditionally, and mac80211's iface.c only ever
 * sets that hw conf bit on the *virtual*-monitor path, which
 * NO_VIRTUAL_MONITOR skips entirely - so the bit is always clear here
 * and that check silently never fires for a real monitor vif. Checking
 * vif->type directly is what mt7925_assign_vif_chanctx() already does
 * for the same reason.
 */
void
mt7925_monitor_reset_iter(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt792x_dev *dev = priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_chanctx_conf *ctx = mvif->bss_conf.mt76.ctx;

	if (vif->type != NL80211_IFTYPE_MONITOR || !ctx)
		return;

	mt7925_monitor_arm_sniffer(&dev->phy, vif, ctx);
}

void mt7925_set_runtime_pm(struct mt792x_dev *dev)
{
	struct ieee80211_hw *hw = mt76_hw(dev);
	struct mt76_connac_pm *pm = &dev->pm;
	bool monitor = !!(hw->conf.flags & IEEE80211_CONF_MONITOR);

	pm->enable = pm->enable_user && !monitor;
	ieee80211_iterate_active_interfaces(hw,
					    IEEE80211_IFACE_ITER_RESUME_ALL,
					    mt7925_pm_interface_iter, dev);
	pm->ds_enable = pm->ds_enable_user && !monitor;
	mt7925_mcu_set_deep_sleep(dev, pm->ds_enable);
}

/* compat: radio_idx added to ieee80211_ops in kernel 6.17 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static int mt7925_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
#else
static int mt7925_config(struct ieee80211_hw *hw, u32 changed)
#endif
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	int ret = 0;

	mt792x_mutex_acquire(dev);

	if (changed & IEEE80211_CONF_CHANGE_POWER) {
		ret = mt7925_set_tx_sar_pwr(hw, NULL);
		if (ret)
			goto out;
	}

	if (changed & IEEE80211_CONF_CHANGE_MONITOR) {
		ieee80211_iterate_active_interfaces_mtx(hw,
							IEEE80211_IFACE_ITER_RESUME_ALL,
							mt7925_sniffer_interface_iter, dev);
	}

out:
	mt792x_mutex_release(dev);

	return ret;
}

static void mt7925_configure_filter(struct ieee80211_hw *hw,
				    unsigned int changed_flags,
				    unsigned int *total_flags,
				    u64 multicast)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	u32 flags = MT7925_FILTER_ENABLE;

#define MT7925_FILTER(_fif, _type) do {			\
		if (*total_flags & (_fif))		\
			flags |= MT7925_FILTER_##_type;	\
	} while (0)

	MT7925_FILTER(FIF_FCSFAIL, FCSFAIL);
	MT7925_FILTER(FIF_CONTROL, CONTROL);
	MT7925_FILTER(FIF_OTHER_BSS, OTHER_BSS);

	phy->rxfilter = flags;

	mt792x_mutex_acquire(dev);
	mt7925_mcu_set_rxfilter(dev, flags, 0, 0);
	if (is_mt7927(&dev->mt76) &&
	    (hw->conf.flags & IEEE80211_CONF_MONITOR))
		ieee80211_iterate_active_interfaces(hw,
					    IEEE80211_IFACE_ITER_RESUME_ALL,
					    mt7925_sniffer_interface_iter, dev);
	mt792x_mutex_release(dev);

	*total_flags &= (FIF_OTHER_BSS | FIF_FCSFAIL | FIF_CONTROL);
}

static u8
mt7925_get_rates_table(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       bool beacon, bool mcast)
{
	struct mt76_vif_link *mvif = (struct mt76_vif_link *)vif->drv_priv;
	struct mt76_phy *mphy = hw->priv;
	u16 rate;
	u8 i, idx, ht;

	rate = mt76_connac2_mac_tx_rate_val(mphy, &vif->bss_conf, beacon, mcast);
	ht = FIELD_GET(MT_TX_RATE_MODE, rate) > MT_PHY_TYPE_OFDM;

	if (beacon && ht) {
		struct mt792x_dev *dev = mt792x_hw_dev(hw);

		/* must odd index */
		idx = MT7925_BEACON_RATES_TBL + 2 * (mvif->idx % 20);
		mt7925_mac_set_fixed_rate_table(dev, idx, rate);
		return idx;
	}

	idx = FIELD_GET(MT_TX_RATE_IDX, rate);
	for (i = 0; i < ARRAY_SIZE(mt76_rates); i++)
		if ((mt76_rates[i].hw_value & GENMASK(7, 0)) == idx)
			return MT792x_BASIC_RATES_TBL + i;

	return mvif->basic_rates_idx;
}






/* compat: radio_idx added to ieee80211_ops in kernel 6.17 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static int mt7925_set_rts_threshold(struct ieee80211_hw *hw, int radio_idx,
				    u32 val)
#else
static int mt7925_set_rts_threshold(struct ieee80211_hw *hw, u32 val)
#endif
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);

	mt792x_mutex_acquire(dev);
	mt7925_mcu_set_rts_thresh(&dev->phy, val);
	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_ampdu_action(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		    struct ieee80211_ampdu_params *params)
{
	return mt7925_mlo_tx_ampdu_action(hw, vif, params);
}


void mt7925_scan_work(struct work_struct *work)
{
	struct mt792x_phy *phy;
	struct mt792x_dev *dev;
	struct mt76_connac_pm *pm;

	phy = (struct mt792x_phy *)container_of(work, struct mt792x_phy,
						scan_work.work);

	dev = phy->dev;
	pm = &dev->pm;

	if (pm->suspended)
		return;

	while (true) {
		struct sk_buff *skb;
		struct tlv *tlv;
		u32 tlv_len;

		spin_lock_bh(&phy->dev->mt76.lock);
		skb = __skb_dequeue(&phy->scan_event_list);
		spin_unlock_bh(&phy->dev->mt76.lock);

		if (!skb)
			break;

		skb_pull(skb, sizeof(struct mt7925_mcu_rxd) + 4);
		tlv = (struct tlv *)skb->data;
		tlv_len = skb->len;

		mt7925_for_each_tlv(tlv, tlv_len) {
			struct mt7925_mcu_scan_chinfo_event *evt;

			switch (le16_to_cpu(tlv->tag)) {
			case UNI_EVENT_SCAN_DONE_BASIC:
				if (test_and_clear_bit(MT76_HW_SCANNING, &phy->mt76->state)) {
					struct cfg80211_scan_info info = {
						.aborted = false,
					};
					ieee80211_scan_completed(phy->mt76->hw, &info);
				}
				break;
			case UNI_EVENT_SCAN_DONE_CHNLINFO:
				if (le16_to_cpu(tlv->len) < sizeof(*tlv) + sizeof(*evt))
					break;

				evt = (struct mt7925_mcu_scan_chinfo_event *)tlv->data;

				mt7925_regd_change(phy, evt->alpha2);

				break;
			case UNI_EVENT_SCAN_DONE_NLO:
				ieee80211_sched_scan_results(phy->mt76->hw);
				break;
			default:
				break;
			}
		}

		dev_kfree_skb(skb);
	}
}

static int
mt7925_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	       struct ieee80211_scan_request *req)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt76_phy *mphy = hw->priv;
	int err;

	mt792x_mutex_acquire(dev);
	err = mt7925_mcu_hw_scan(mphy, vif, req);
	mt792x_mutex_release(dev);

	return err;
}

static void
mt7925_cancel_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt76_phy *mphy = hw->priv;

	mt792x_mutex_acquire(dev);
	mt7925_mcu_cancel_hw_scan(mphy, vif);
	mt792x_mutex_release(dev);
}

static int
mt7925_start_sched_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			struct cfg80211_sched_scan_request *req,
			struct ieee80211_scan_ies *ies)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt76_phy *mphy = hw->priv;
	int err;

	mt792x_mutex_acquire(dev);

	err = mt7925_mcu_sched_scan_req(mphy, vif, req, ies);
	if (err < 0)
		goto out;

	err = mt7925_mcu_sched_scan_enable(mphy, vif, true);
out:
	mt792x_mutex_release(dev);

	return err;
}

static int
mt7925_stop_sched_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt76_phy *mphy = hw->priv;
	int err;

	mt792x_mutex_acquire(dev);
	err = mt7925_mcu_sched_scan_enable(mphy, vif, false);
	mt792x_mutex_release(dev);

	return err;
}

/* compat: radio_idx added to ieee80211_ops in kernel 6.17 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static int
mt7925_set_antenna(struct ieee80211_hw *hw, int radio_idx,
		   u32 tx_ant, u32 rx_ant)
#else
static int
mt7925_set_antenna(struct ieee80211_hw *hw, u32 tx_ant, u32 rx_ant)
#endif
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	int max_nss = hweight8(hw->wiphy->available_antennas_tx);

	if (!tx_ant || tx_ant != rx_ant || ffs(tx_ant) > max_nss)
		return -EINVAL;

	if ((BIT(hweight8(tx_ant)) - 1) != tx_ant)
		tx_ant = BIT(ffs(tx_ant) - 1) - 1;

	mt792x_mutex_acquire(dev);

	phy->mt76->antenna_mask = tx_ant;
	phy->mt76->chainmask = tx_ant;

	mt76_set_stream_caps(phy->mt76, true);
	mt7925_set_stream_he_eht_caps(phy);

	/* TODO: update bmc_wtbl spe_idx when antenna changes */
	mt792x_mutex_release(dev);

	return 0;
}

#ifdef CONFIG_PM
static int mt7925_suspend(struct ieee80211_hw *hw,
			  struct cfg80211_wowlan *wowlan)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_phy *phy = mt792x_hw_phy(hw);

	ieee80211_iterate_active_interfaces_mtx(hw, 0,
					      mt7925_mlo_quiesce_iter, NULL);
	cancel_delayed_work_sync(&phy->scan_work);
	cancel_delayed_work_sync(&phy->mt76->mac_work);

	cancel_delayed_work_sync(&dev->pm.ps_work);
	cancel_delayed_work_sync(&dev->mlo_pm_work);
	mt76_connac_free_pending_tx_skbs(&dev->pm, NULL);

	mt792x_mutex_acquire(dev);

	clear_bit(MT76_STATE_RUNNING, &phy->mt76->state);
	ieee80211_iterate_active_interfaces_mtx(hw,
						IEEE80211_IFACE_ITER_RESUME_ALL,
						mt7925_mcu_set_suspend_iter,
						&dev->mphy);

	mt792x_mutex_release(dev);

	return 0;
}

static int mt7925_resume(struct ieee80211_hw *hw)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_phy *phy = mt792x_hw_phy(hw);

	mt792x_mutex_acquire(dev);

	set_bit(MT76_STATE_RUNNING, &phy->mt76->state);
	ieee80211_iterate_active_interfaces_mtx(hw,
						IEEE80211_IFACE_ITER_RESUME_ALL,
						mt7925_mcu_set_suspend_iter,
						&dev->mphy);

	ieee80211_queue_delayed_work(hw, &phy->mt76->mac_work,
				     MT792x_WATCHDOG_TIME);

	mt792x_mutex_release(dev);
	ieee80211_iterate_active_interfaces_mtx(hw, 0,
					      mt7925_mlo_resume_iter, hw);

	return 0;
}

static void mt7925_set_rekey_data(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  struct cfg80211_gtk_rekey_data *data)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);

	mt792x_mutex_acquire(dev);
	mt76_connac_mcu_update_gtk_rekey(hw, vif, data);
	mt792x_mutex_release(dev);
}
#endif /* CONFIG_PM */

static void mt7925_sta_set_decap_offload(struct ieee80211_hw *hw,
					 struct ieee80211_vif *vif,
					 struct ieee80211_sta *sta,
					 bool enabled)
{
	struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	unsigned long valid = mvif->valid_links;
	u8 i;

	if (!msta->vif)
		return;

	mt792x_mutex_acquire(dev);

	valid = ieee80211_vif_is_mld(vif) ? mvif->valid_links : BIT(0);

	for_each_set_bit(i, &valid, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *mconf;
		struct mt792x_link_sta *mlink;

		mconf = mt792x_vif_to_link(mvif, i);
		mlink = mt792x_sta_to_link(msta, i);

		if (!mlink)
			continue;

		if (enabled)
			set_bit(MT_WCID_FLAG_HDR_TRANS, &mlink->wcid.flags);
		else
			clear_bit(MT_WCID_FLAG_HDR_TRANS, &mlink->wcid.flags);

		if (!mlink->wcid.sta)
			continue;

		mt7925_mcu_wtbl_update_hdr_trans(dev, vif, mconf, mlink);
	}

	mt792x_mutex_release(dev);
}

#if IS_ENABLED(CONFIG_IPV6)
static void __mt7925_ipv6_addr_change(struct ieee80211_hw *hw,
				      struct ieee80211_bss_conf *link_conf,
				      struct inet6_dev *idev)
{
	struct mt792x_bss_conf *mconf = mt792x_link_conf_to_mconf(link_conf);
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct inet6_ifaddr *ifa;
	struct sk_buff *skb;
	u8 idx = 0;

	struct {
		struct {
			u8 bss_idx;
			u8 pad[3];
		} __packed hdr;
		struct mt7925_arpns_tlv arpns;
		struct in6_addr ns_addrs[IEEE80211_BSS_ARP_ADDR_LIST_LEN];
	} req_hdr = {
		.hdr = {
			.bss_idx = mconf->mt76.idx,
		},
		.arpns = {
			.tag = cpu_to_le16(UNI_OFFLOAD_OFFLOAD_ND),
			.len = cpu_to_le16(sizeof(req_hdr) - 4),
			.enable = true,
		},
	};

	read_lock_bh(&idev->lock);
	list_for_each_entry(ifa, &idev->addr_list, if_list) {
		if (ifa->flags & IFA_F_TENTATIVE)
			continue;
		req_hdr.ns_addrs[idx] = ifa->addr;
		if (++idx >= IEEE80211_BSS_ARP_ADDR_LIST_LEN)
			break;
	}
	read_unlock_bh(&idev->lock);

	if (!idx)
		return;

	req_hdr.arpns.ips_num = idx;

	skb = __mt76_mcu_msg_alloc(&dev->mt76, NULL, sizeof(req_hdr),
				   0, GFP_ATOMIC);
	if (!skb)
		return;

	skb_put_data(skb, &req_hdr, sizeof(req_hdr));

	skb_queue_tail(&dev->ipv6_ns_list, skb);

	ieee80211_queue_work(dev->mt76.hw, &dev->ipv6_ns_work);
}

static void mt7925_ipv6_addr_change(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct inet6_dev *idev)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long valid = ieee80211_vif_is_mld(vif) ?
			      mvif->valid_links : BIT(0);
	struct ieee80211_bss_conf *bss_conf;
	int i;

	for_each_set_bit(i, &valid, IEEE80211_MLD_MAX_NUM_LINKS) {
		bss_conf = mt792x_vif_to_bss_conf(vif, i);
		__mt7925_ipv6_addr_change(hw, bss_conf, idev);
	}
}

#endif

int mt7925_set_tx_sar_pwr(struct ieee80211_hw *hw,
			  const struct cfg80211_sar_specs *sar)
{
	struct mt76_phy *mphy = hw->priv;

	if (sar) {
		int err = mt76_init_sar_power(hw, sar);

		if (err)
			return err;
	}
	mt792x_init_acpi_sar_power(mt792x_hw_phy(hw), !sar);

	return mt7925_mcu_set_rate_txpower(mphy);
}

static int mt7925_set_sar_specs(struct ieee80211_hw *hw,
				const struct cfg80211_sar_specs *sar)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	int err;

	mt792x_mutex_acquire(dev);
	err = mt7925_mcu_set_clc(dev, dev->mt76.alpha2,
			 dev->country_ie_env);
	if (err < 0)
		goto out;

	err = mt7925_set_tx_sar_pwr(hw, sar);

out:
	mt792x_mutex_release(dev);
	return err;
}

static void
mt7925_channel_switch_beacon(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif,
			     struct cfg80211_chan_def *chandef)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);

	mt792x_mutex_acquire(dev);
	mt7925_mcu_uni_add_beacon_offload(dev, hw, vif, true);
	mt792x_mutex_release(dev);
}

static int
mt7925_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	       unsigned int link_id, u16 queue,
	       const struct ieee80211_tx_queue_params *params)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_bss_conf *mconf = mt792x_vif_to_link(mvif, link_id);
	static const u8 mq_to_aci[] = {
		    [IEEE80211_AC_VO] = 3,
		    [IEEE80211_AC_VI] = 2,
		    [IEEE80211_AC_BE] = 0,
		    [IEEE80211_AC_BK] = 1,
	};

	/* firmware uses access class index */
	mconf->queue_params[mq_to_aci[queue]] = *params;

	return 0;
}

static int
mt7925_start_ap(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		struct ieee80211_bss_conf *link_conf)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	int err;

	mt792x_mutex_acquire(dev);

	err = mt7925_mcu_add_bss_info(&dev->phy, mvif->bss_conf.mt76.ctx,
				      link_conf, NULL, true);
	if (err)
		goto out;

	err = mt7925_mcu_set_bss_pm(dev, link_conf, true);
	if (err)
		goto out;

	err = mt7925_mcu_sta_update(dev, NULL, vif,
				    &mvif->sta.deflink, true,
				    MT76_STA_INFO_STATE_NONE, NULL);
out:
	mt792x_mutex_release(dev);

	return err;
}

static void
mt7925_stop_ap(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	       struct ieee80211_bss_conf *link_conf)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	int err;

	mt792x_mutex_acquire(dev);

	err = mt7925_mcu_set_bss_pm(dev, link_conf, false);
	if (err)
		goto out;

	mt7925_mcu_add_bss_info(&dev->phy, mvif->bss_conf.mt76.ctx, link_conf,
				NULL, false);

out:
	mt792x_mutex_release(dev);
}

static int
mt7925_add_chanctx(struct ieee80211_hw *hw,
		   struct ieee80211_chanctx_conf *ctx)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);

	dev->new_ctx = ctx;

	return 0;
}

static void
mt7925_remove_chanctx(struct ieee80211_hw *hw,
		      struct ieee80211_chanctx_conf *ctx)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);

	if (dev->new_ctx == ctx)
		dev->new_ctx = NULL;

}

static void
mt7925_change_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx,
		u32 changed)
{
	mt7927_mlo_assoc_change_chanctx(hw, ctx, changed);
}

static void mt7925_mgd_prepare_tx(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  struct ieee80211_prep_tx_info *info)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	u16 duration = info->duration ? info->duration :
		       jiffies_to_msecs(HZ);

	mt792x_mutex_acquire(dev);
	/* mvif->bss_conf.mt76.ctx can already be NULL here: deauth/disassoc
	 * teardown (ieee80211_set_disassoc() -> drv_mgd_prepare_tx()) can
	 * race with this vif's chanctx already having been unassigned by an
	 * MLO link-mask transition (mt7925_change_vif_links() teardown of
	 * the deflink's own bss_conf), so there is nothing to ROC-join for.
	 */
	if (mvif->bss_conf.mt76.ctx)
		mt7925_set_roc(mvif->phy, &mvif->bss_conf,
			       mvif->bss_conf.mt76.ctx->def.chan, duration,
			       MT7925_ROC_REQ_JOIN);
	mt792x_mutex_release(dev);
}

static void mt7925_mgd_complete_tx(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   struct ieee80211_prep_tx_info *info)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;

	/* Keep the join ROC until the station is authorized, so that it
	 * covers the 4-way handshake. mt7925_mac_sta_event() releases it.
	 * On MLD, the active ROC is the MLO ROC, which must be released
	 * before the link activation work sets its own.
	 */
	if (vif->type == NL80211_IFTYPE_STATION && !ieee80211_vif_is_mld(vif) &&
	    info->success &&
	    (info->subtype == IEEE80211_STYPE_ASSOC_REQ ||
	     info->subtype == IEEE80211_STYPE_REASSOC_REQ)) {
		mvif->roc_join_held = true;
		return;
	}

	mt7925_abort_roc(mvif->phy, &mvif->bss_conf);
}

static void
mt7925_vif_cfg_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u64 changed)
{
	mt7927_mlo_assoc_vif_cfg_changed(hw, vif, changed);
}

static void mt7925_link_info_changed(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *info,
				     u64 changed)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_bss_conf *mconf;

	mconf = mt792x_vif_to_link(mvif, info->link_id);

	mt792x_mutex_acquire(dev);

	if (changed & BSS_CHANGED_ERP_SLOT) {
		int slottime = info->use_short_slot ? 9 : 20;

		if (slottime != phy->slottime) {
			phy->slottime = slottime;
			mt7925_mcu_set_timing(phy, info);
		}
	}

	if (changed & BSS_CHANGED_MCAST_RATE)
		mconf->mt76.mcast_rates_idx =
				mt7925_get_rates_table(hw, vif, false, true);

	if (changed & BSS_CHANGED_BASIC_RATES)
		mconf->mt76.basic_rates_idx =
				mt7925_get_rates_table(hw, vif, false, false);

	if (changed & (BSS_CHANGED_BEACON |
		       BSS_CHANGED_BEACON_ENABLED)) {
		mconf->mt76.beacon_rates_idx =
				mt7925_get_rates_table(hw, vif, true, false);

		mt7925_mcu_uni_add_beacon_offload(dev, hw, vif,
						  info->enable_beacon);
	}

	/* ensure that enable txcmd_mode after bss_info */
	if (changed & (BSS_CHANGED_QOS | BSS_CHANGED_BEACON_ENABLED))
		mt7925_mcu_set_tx(dev, info);

	mt7927_mlo_assoc_link_info_changed(hw, vif, info);

	if (changed & BSS_CHANGED_CQM)
		mt7925_mcu_set_rssimonitor(dev, vif);

	mt792x_mutex_release(dev);
}

static int
mt7925_change_vif_links(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		u16 old_links, u16 new_links,
		struct ieee80211_bss_conf *old[IEEE80211_MLD_MAX_NUM_LINKS])
{
	return mt7927_mlo_assoc_change_vif_links(hw, vif, old_links, new_links, old);
}

static int
mt7925_change_sta_links(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		struct ieee80211_sta *sta, u16 old_links, u16 new_links)
{
	return mt7927_mlo_assoc_change_sta_links(hw, vif, sta, old_links, new_links);
}


static int
mt7925_assign_vif_chanctx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		struct ieee80211_bss_conf *link_conf, struct ieee80211_chanctx_conf *ctx)
{
	return mt7927_mlo_assoc_assign_vif_chanctx(hw, vif, link_conf, ctx);
}

static void
mt7925_unassign_vif_chanctx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		struct ieee80211_bss_conf *link_conf, struct ieee80211_chanctx_conf *ctx)
{
	mt7927_mlo_assoc_unassign_vif_chanctx(hw, vif, link_conf, ctx);
}

static void mt7925_rfkill_poll(struct ieee80211_hw *hw)
{
	struct mt792x_phy *phy = mt792x_hw_phy(hw);
	int ret;

	mt792x_mutex_acquire(phy->dev);
	ret = mt7925_mcu_wf_rf_pin_ctrl(phy);
	mt792x_mutex_release(phy->dev);

	wiphy_rfkill_set_hw_state(hw->wiphy, ret == 0);
}

static int mt7925_switch_vif_chanctx(struct ieee80211_hw *hw,
				     struct ieee80211_vif_chanctx_switch *vifs,
				     int n_vifs,
				     enum ieee80211_chanctx_switch_mode mode)
{
	return mt7925_assign_vif_chanctx(hw, vifs->vif, vifs->link_conf,
					 vifs->new_ctx);
}

void mt7925_csa_work(struct work_struct *work)
{
	struct mt792x_vif *mvif;
	struct mt792x_dev *dev;
	struct ieee80211_vif *vif;
	struct ieee80211_bss_conf *link_conf;
	struct mt792x_bss_conf *mconf;
	u8 link_id, roc_rtype;
	int ret = 0;

	mvif = (struct mt792x_vif *)container_of(work, struct mt792x_vif,
						csa_work);
	dev = mvif->phy->dev;
	vif = container_of((void *)mvif, struct ieee80211_vif, drv_priv);

	if (ieee80211_vif_is_mld(vif))
		return;

	if (!dev->new_ctx)
		return;

	link_id = 0;
	mconf = &mvif->bss_conf;
	link_conf = &vif->bss_conf;
	roc_rtype = MT7925_ROC_REQ_JOIN;

	mt792x_mutex_acquire(dev);
	ret = mt7925_set_roc(mvif->phy, mconf, dev->new_ctx->def.chan,
			     4000, roc_rtype);
	mt792x_mutex_release(dev);
	if (!ret) {
		mt792x_mutex_acquire(dev);
		ret = mt7925_mcu_set_chctx(mvif->phy->mt76, &mconf->mt76, link_conf,
					   dev->new_ctx);
		mt792x_mutex_release(dev);

		mt7925_abort_roc(mvif->phy, mconf);
	}

	ieee80211_chswitch_done(vif, !ret, link_id);
}

static int mt7925_pre_channel_switch(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_channel_switch *chsw)
{
	if (ieee80211_vif_is_mld(vif))
		return -EOPNOTSUPP;

	if (vif->type != NL80211_IFTYPE_STATION || !vif->cfg.assoc)
		return -EOPNOTSUPP;

	if (!cfg80211_chandef_usable(hw->wiphy, &chsw->chandef,
				     IEEE80211_CHAN_DISABLED))
		return -EOPNOTSUPP;

	return 0;
}

static void mt7925_channel_switch(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  struct ieee80211_channel_switch *chsw)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	u16 beacon_interval;

	if (ieee80211_vif_is_mld(vif))
		return;

	beacon_interval = vif->bss_conf.beacon_int;

	mvif->csa_timer.expires = TU_TO_EXP_TIME(beacon_interval * chsw->count);
	add_timer(&mvif->csa_timer);
}

static void mt7925_abort_channel_switch(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif,
					struct ieee80211_bss_conf *link_conf)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;

	timer_delete_sync(&mvif->csa_timer);
	cancel_work_sync(&mvif->csa_work);
}

static void mt7925_channel_switch_rx_beacon(struct ieee80211_hw *hw,
					    struct ieee80211_vif *vif,
					    struct ieee80211_channel_switch *chsw)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	u16 beacon_interval;

	if (ieee80211_vif_is_mld(vif))
		return;

	if (!dev->new_ctx)
		return;

	beacon_interval = vif->bss_conf.beacon_int;

	if (cfg80211_chandef_identical(&chsw->chandef,
				       &dev->new_ctx->def) &&
				       chsw->count) {
		mod_timer(&mvif->csa_timer,
			  TU_TO_EXP_TIME(beacon_interval * chsw->count));
	}
}

static void mt7925_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);

	ieee80211_iterate_active_interfaces_mtx(hw, 0,
					      mt7925_mlo_quiesce_iter, NULL);
	cancel_delayed_work_sync(&dev->mlo_pm_work);

	mt792x_stop(hw, suspend);
}

/* compat: the cfg80211 NAN API arrived in kernel 7.2 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
static int mt7925_start_nan(struct ieee80211_hw *hw,
			    struct ieee80211_vif *vif,
			    struct cfg80211_nan_conf *conf)
{
	struct ieee80211_bss_conf *link_conf = &vif->bss_conf;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct ieee80211_channel *chan;
	int err = 0;

	mt792x_mutex_acquire(dev);

	chan = conf->band_cfgs[NL80211_BAND_2GHZ].chan;
	if (!chan) {
		err = -EINVAL;
		goto out;
	}

	cfg80211_chandef_create(&link_conf->chanreq.oper, chan,
				NL80211_CHAN_NO_HT);

	err = mt7925_mcu_add_bss_info(&dev->phy, NULL,
				      link_conf, NULL, true);
	if (err < 0)
		goto out;

	err = mt7925_nan_enable(vif, dev, conf);
	if (err) {
		mt7925_mcu_add_bss_info(&dev->phy, NULL, link_conf, NULL, false);
		goto out;
	}

	err = mt7925_nan_update_phy_setting(dev);

out:
	mt792x_mutex_release(dev);

	return err;
}

static int mt7925_stop_nan(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif)
{
	struct ieee80211_bss_conf *link_conf = &vif->bss_conf;
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	int err, ret;

	/* Drop a deferred event queued just before stop so a stale cluster_id
	 * cannot leak into a restart; the work re-checks liveness anyway.
	 */
	spin_lock_bh(&dev->nan_deferred_lock);
	dev->nan_deferred_pending = 0;
	spin_unlock_bh(&dev->nan_deferred_lock);

	mt792x_mutex_acquire(dev);

	err = mt7925_nan_disable(vif, dev);

	ret = mt7925_mcu_add_bss_info(&dev->phy, NULL, link_conf,
				      NULL, false);
	if (!err)
		err = ret;

	mt792x_mutex_release(dev);

	return err;
}

static int mt7925_nan_change_conf(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  struct cfg80211_nan_conf *conf,
				  u32 changes)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	int err = 0;

	mt792x_mutex_acquire(dev);

	err = mt7925_nan_change_configure(vif, dev, conf);

	mt792x_mutex_release(dev);

	return err;
}

static int mt7925_nan_peer_sched_changed(struct ieee80211_hw *hw,
					 struct ieee80211_sta *sta)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	int err = 0;

	mt792x_mutex_acquire(dev);

	err = mt792x_nan_set_peer_schedule(dev, sta);

	mt792x_mutex_release(dev);

	return err;
}
#endif

static void mt7925_sta_pre_rcu_remove(struct ieee80211_hw *hw,
				      struct ieee80211_vif *vif,
				      struct ieee80211_sta *sta)
{
	struct mt76_phy *phy = hw->priv;
	struct mt76_dev *dev = phy->dev;
	struct mt76_wcid *wcid = (struct mt76_wcid *)sta->drv_priv;

	mutex_lock(&dev->mutex);
	spin_lock_bh(&dev->status_lock);

	if (ieee80211_vif_is_mld(vif)) {
		struct mt792x_sta *msta = (struct mt792x_sta *)sta->drv_priv;
		struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
		unsigned long valid = mvif->valid_links;
		struct mt792x_link_sta *mlink;
		unsigned int link_id;

		for_each_set_bit(link_id, &valid, IEEE80211_MLD_MAX_NUM_LINKS) {
			mlink = mt792x_sta_to_link(msta, link_id);
			if (!mlink || !mlink->wcid.sta)
				continue;
			if (mlink->wcid.idx < ARRAY_SIZE(dev->wcid))
				rcu_assign_pointer(dev->wcid[mlink->wcid.idx],
						   NULL);
		}
	} else {
		rcu_assign_pointer(dev->wcid[wcid->idx], NULL);
	}

	spin_unlock_bh(&dev->status_lock);
	mutex_unlock(&dev->mutex);
}

const struct ieee80211_ops mt7925_ops = {
	.tx = mt792x_tx,
	.start = mt7925_start,
	.stop = mt7925_stop,
	.add_interface = mt7925_add_interface,
	.remove_interface = mt7925_remove_interface,
	.config = mt7925_config,
	.conf_tx = mt7925_conf_tx,
	.configure_filter = mt7925_configure_filter,
	.start_ap = mt7925_start_ap,
	.stop_ap = mt7925_stop_ap,
	.sta_state = mt76_sta_state,
	.sta_pre_rcu_remove = mt7925_sta_pre_rcu_remove,
	.set_key = mt7925_set_key,
	.sta_set_decap_offload = mt7925_sta_set_decap_offload,
#if IS_ENABLED(CONFIG_IPV6)
	.ipv6_addr_change = mt7925_ipv6_addr_change,
#endif /* CONFIG_IPV6 */
	.ampdu_action = mt7925_ampdu_action,
	.set_rts_threshold = mt7925_set_rts_threshold,
	.wake_tx_queue = mt76_wake_tx_queue,
	.release_buffered_frames = mt76_release_buffered_frames,
	.channel_switch_beacon = mt7925_channel_switch_beacon,
	.get_txpower = mt792x_get_txpower,
	.get_stats = mt792x_get_stats,
	.get_et_sset_count = mt792x_get_et_sset_count,
	.get_et_strings = mt792x_get_et_strings,
	.get_et_stats = mt792x_get_et_stats,
	.get_tsf = mt792x_get_tsf,
	.set_tsf = mt792x_set_tsf,
	.get_survey = mt76_get_survey,
	.get_antenna = mt76_get_antenna,
	.set_antenna = mt7925_set_antenna,
	.set_coverage_class = mt792x_set_coverage_class,
	.hw_scan = mt7925_hw_scan,
	.cancel_hw_scan = mt7925_cancel_hw_scan,
	.sta_statistics = mt792x_sta_statistics,
	.sched_scan_start = mt7925_start_sched_scan,
	.sched_scan_stop = mt7925_stop_sched_scan,
	CFG80211_TESTMODE_CMD(mt7925_testmode_cmd)
	CFG80211_TESTMODE_DUMP(mt7925_testmode_dump)
#ifdef CONFIG_PM
	.suspend = mt7925_suspend,
	.resume = mt7925_resume,
	.set_wakeup = mt792x_set_wakeup,
	.set_rekey_data = mt7925_set_rekey_data,
#endif /* CONFIG_PM */
	.flush = mt792x_flush,
	.set_sar_specs = mt7925_set_sar_specs,
	.remain_on_channel = mt7925_remain_on_channel,
	.cancel_remain_on_channel = mt7925_cancel_remain_on_channel,
	.add_chanctx = mt7925_add_chanctx,
	.remove_chanctx = mt7925_remove_chanctx,
	.change_chanctx = mt7925_change_chanctx,
	.assign_vif_chanctx = mt7925_assign_vif_chanctx,
	.unassign_vif_chanctx = mt7925_unassign_vif_chanctx,
	.mgd_prepare_tx = mt7925_mgd_prepare_tx,
	.mgd_complete_tx = mt7925_mgd_complete_tx,
	.vif_cfg_changed = mt7925_vif_cfg_changed,
	.link_info_changed = mt7925_link_info_changed,
	.change_vif_links = mt7925_change_vif_links,
	.change_sta_links = mt7925_change_sta_links,
	.can_neg_ttlm = mt7925_mlo_can_neg_ttlm,
	.rfkill_poll = mt7925_rfkill_poll,

	.switch_vif_chanctx = mt7925_switch_vif_chanctx,
	.pre_channel_switch = mt7925_pre_channel_switch,
	.channel_switch = mt7925_channel_switch,
	.abort_channel_switch = mt7925_abort_channel_switch,
	.channel_switch_rx_beacon = mt7925_channel_switch_rx_beacon,
/* compat: the cfg80211 NAN API arrived in kernel 7.2 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
	.start_nan = mt7925_start_nan,
	.stop_nan = mt7925_stop_nan,
	.nan_change_conf = mt7925_nan_change_conf,
	.nan_peer_sched_changed = mt7925_nan_peer_sched_changed,
#endif
};
EXPORT_SYMBOL_GPL(mt7925_ops);

MODULE_AUTHOR("Deren Wu <deren.wu@mediatek.com>");
MODULE_DESCRIPTION("MediaTek MT7925 core driver");
MODULE_LICENSE("Dual BSD/GPL");
