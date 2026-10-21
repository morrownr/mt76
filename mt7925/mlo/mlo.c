// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "internal.h"
#include "policy.h"

static bool
mt7925_mlo_dispatch(struct mt792x_phy *phy,
		    struct mt7925_mlo_request *request)
{
	struct mt792x_dev *dev = phy->dev;
	bool mlo_capable = phy->chip_cap & MT792x_CHIP_CAP_MLO_EN;
	bool mt7927 = is_mt7927(&dev->mt76);

	switch (request->action) {
	case MT7925_MLO_ACTION_UPDATE_CAPS:
		if (!request->mld_capa_and_ops ||
		    !mt7925_mlo_device_supported(mt7927, mlo_capable))
			return false;

		mt7925_mlo_ttlm_update_caps(request->mld_capa_and_ops);
		return true;
	case MT7925_MLO_ACTION_NEG_TTLM:
		if (!request->neg_ttlm.vif || !request->neg_ttlm.ttlm ||
		    !mt7925_mlo_request_eligible(mt7927, mlo_capable,
			    request->neg_ttlm.vif->type ==
				    NL80211_IFTYPE_STATION,
			    ieee80211_vif_is_mld(request->neg_ttlm.vif)))
			return false;

		return mt7925_mlo_ttlm_validate(request->neg_ttlm.vif,
						request->neg_ttlm.ttlm);
	default:
		return false;
	}
}

void mt7925_mlo_update_caps(struct mt792x_phy *phy, u16 *mld_capa_and_ops)
{
	struct mt7925_mlo_request request = {
		.action = MT7925_MLO_ACTION_UPDATE_CAPS,
		.mld_capa_and_ops = mld_capa_and_ops,
	};

	mt7925_mlo_dispatch(phy, &request);
}

enum ieee80211_neg_ttlm_res
mt7925_mlo_can_neg_ttlm(struct ieee80211_hw *hw,
			struct ieee80211_vif *vif,
			struct ieee80211_neg_ttlm *ttlm)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);
	struct mt7925_mlo_request request = {
		.action = MT7925_MLO_ACTION_NEG_TTLM,
		.neg_ttlm = {
			.vif = vif,
			.ttlm = ttlm,
		},
	};

	if (!mt7925_mlo_dispatch(&dev->phy, &request))
		return NEG_TTLM_RES_REJECT;

	return NEG_TTLM_RES_ACCEPT;
}

/* Firmware MLO capability record (NIC_CAP tag 0x22), byte for byte as the
 * vendor driver and the Windows nicCfgChipCapMlo() read it. Called by the
 * generic NIC_CAP TLV loop, which is the integration boundary.
 */
void mt7925_mlo_fw_cap_parse(struct mt792x_dev *dev, const u8 *data, int len)
{
	const struct mt7925_mlo_fw_cap_raw {
		u8 nonap_eml_support;
		u8 ap_eml_support;
		u8 max_simul_links;
		u8 link3_band_limit;
		__le16 nonap_eml_cap;
		__le16 ap_eml_cap;
		u8 hybrid_mlo;
		u8 rsv[3];
	} __packed *cap = (const void *)data;
	struct mt792x_phy *phy = &dev->phy;

	if (len < sizeof(*cap))
		return;

	phy->mlo_fw_cap.valid = true;
	phy->mlo_fw_cap.nonap_eml_support = cap->nonap_eml_support;
	phy->mlo_fw_cap.ap_eml_support = cap->ap_eml_support;
	phy->mlo_fw_cap.max_simul_links = cap->max_simul_links;
	phy->mlo_fw_cap.link3_band_limit = cap->link3_band_limit;
	phy->mlo_fw_cap.hybrid_mlo = cap->hybrid_mlo;
	phy->mlo_fw_cap.nonap_eml_cap = le16_to_cpu(cap->nonap_eml_cap);
	phy->mlo_fw_cap.ap_eml_cap = le16_to_cpu(cap->ap_eml_cap);
	memcpy(phy->mlo_fw_cap.raw, data, sizeof(phy->mlo_fw_cap.raw));

	/* the advertised EML capability is the firmware's non-AP EML field */
	phy->eml_cap = phy->mlo_fw_cap.nonap_eml_cap;

	if (is_mt7927(&dev->mt76))
		dev_info(dev->mt76.dev,
			 "MT7927_FW_MLO_CAP: nonap_eml_support=%u nonap_eml_cap=0x%04x ap_eml_support=%u ap_eml_cap=0x%04x max_simul_links=%u link3_band_limit=0x%02x hybrid=%u raw=%*phN\n",
			 cap->nonap_eml_support, phy->mlo_fw_cap.nonap_eml_cap,
			 cap->ap_eml_support, phy->mlo_fw_cap.ap_eml_cap,
			 cap->max_simul_links, cap->link3_band_limit,
			 cap->hybrid_mlo, (int)sizeof(phy->mlo_fw_cap.raw), data);
}

int mt7925_init_mlo_caps(struct mt792x_phy *phy)
{
	struct wiphy *wiphy = phy->mt76->hw->wiphy;
	struct wiphy_iftype_ext_capab *ext_capab;
	u8 simul_links;
	static const u8 ext_capa_sta[] = {
		[0] = WLAN_EXT_CAPA1_EXT_CHANNEL_SWITCHING,
		[2] = WLAN_EXT_CAPA3_MULTI_BSSID_SUPPORT,
		[7] = WLAN_EXT_CAPA8_OPMODE_NOTIF,
	};
	static const struct wiphy_iftype_ext_capab ext_capab_template[] = {
		{
			.iftype = NL80211_IFTYPE_STATION,
			.extended_capabilities = ext_capa_sta,
			.extended_capabilities_mask = ext_capa_sta,
			.extended_capabilities_len = sizeof(ext_capa_sta),
		},
	};

	if (!(phy->chip_cap & MT792x_CHIP_CAP_MLO_EN))
		return 0;

	ext_capab = devm_kmemdup(phy->dev->mt76.dev, ext_capab_template,
				 sizeof(ext_capab_template), GFP_KERNEL);
	if (!ext_capab)
		return -ENOMEM;

	ext_capab[0].eml_capabilities = phy->eml_cap;
	pr_info("mt7925: MLO_EML_CAP_ADVERTISE: raw=0x%04x emlsr_supp=%d\n",
		phy->eml_cap, !!(phy->eml_cap & IEEE80211_EML_CAP_EMLSR_SUPP));
	/* IEEE 802.11be encodes "Maximum Number of Simultaneous Links" as
	 * (N - 1), so 0 advertises a single simultaneous link and 1
	 * advertises two. MT7927 is dual-band-concurrent (DBDC is enabled at
	 * init and the vendor Windows driver runs two links simultaneously on
	 * this silicon), so 1 is the value the hardware should be able to
	 * honour.
	 *
	 * Runtime-selectable so the behaviour can be A/B tested without a
	 * rebuild. Default stays at the upstream-safe 0 until the two-link
	 * path is proven.
	 */
	simul_links = mt7925_mlo_max_simul_links;
	if (simul_links == MT7925_MLO_SIMUL_LINKS_AUTO) {
		/* MT7927 is dual-band concurrent: DBDC is enabled at init and
		 * the firmware grants a multi-link (JOIN) ROC covering both
		 * bands, so it can genuinely hold two links at once. Every
		 * other mt7925-class part is single-link (MLSR) only.
		 */
		simul_links = is_mt7927(&phy->dev->mt76) ? 1 : 0;
	}

	ext_capab[0].mld_capa_and_ops =
		u16_encode_bits(simul_links & 0xf,
				IEEE80211_MLD_CAP_OP_MAX_SIMUL_LINKS);
	mt7925_mlo_update_caps(phy, &ext_capab[0].mld_capa_and_ops);

	wiphy->flags |= WIPHY_FLAG_SUPPORTS_MLO;
	wiphy->iftype_ext_capab = ext_capab;
	wiphy->num_iftype_ext_capab = ARRAY_SIZE(ext_capab_template);

	return 0;
}
