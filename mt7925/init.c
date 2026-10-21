// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2023 MediaTek Inc. */

#include <linux/etherdevice.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/thermal.h>
#include <linux/firmware.h>
#include "mt7925.h"
#include "mlo/pairs.h"
#include "regd.h"
#include "mac.h"
#include "mcu.h"

/* MT7927 DBDC/MBMC provisioning.
 *
 * The MediaTek Windows driver (mtkwecx.sys) sends the MBMC/DBDC enable
 * command (UNI CID 0x28, UNI_MBMC_SETTING) only for MT7925 (0x7925) and
 * MT7921-class (0x717) parts, always paired with an antenna-path change
 * that halves NSS from 2x2 to 1x1 -- i.e. it splits a single 2x2 die into
 * two 1x1 band chains so that two links can be held concurrently.
 * MT7927 (0x7927) is excluded from every DBDC decision path in that driver
 * and is instead provisioned via the INF with StaMloMode=1 (verified on
 * live hardware: adapter registry reads StaMloMode=dword:00000001), which
 * pins it to exactly two links without any DBDC split.
 *
 * Linux does the inverse: it enables DBDC unconditionally and exclusively
 * for MT7927, and never performs the paired NSS adjustment.
 *
 * TESTED 2026-08-10 on MT7927 (PCI 14c3:7927), AP UniFi UCG-UX7:
 *   mt7927_dbdc=0 -> 0/3 associations succeeded (SME auth times out)
 *   mt7927_dbdc=1 -> 3/3 associations succeeded, 3 links each
 * DBDC is therefore REQUIRED for MT7927 to associate under Linux; the
 * Windows divergence does not transfer. Default stays enabled. The
 * parameter is retained only to re-run that A/B experiment.
 */
bool mt7925_mt7927_dbdc = true;
EXPORT_SYMBOL_GPL(mt7925_mt7927_dbdc);
module_param_named(mt7927_dbdc, mt7925_mt7927_dbdc, bool, 0644);
MODULE_PARM_DESC(mt7927_dbdc,
		 "enable DBDC/MBMC on MT7927 (default 1; 0 breaks association - test only)");

/* IEEE 802.11be "Maximum Number of Simultaneous Links" is encoded as (N - 1).
 * 0 => one simultaneous link (upstream default), 1 => two.
 */
u8 mt7925_mlo_max_simul_links = MT7925_MLO_SIMUL_LINKS_AUTO;
EXPORT_SYMBOL_GPL(mt7925_mlo_max_simul_links);
module_param_named(mlo_max_simul_links, mt7925_mlo_max_simul_links, byte, 0644);
MODULE_PARM_DESC(mlo_max_simul_links,
		 "advertised MLD max simultaneous links, encoded as N-1; 255 = per-chip default");

/* Release multi-link (JOIN) ROC reservations back to the firmware. Test-only
 * switch: turning this off restores the previous behaviour, where the
 * reservation is leaked and the next multi-link ROC request is never granted.
 */
u8 mt7925_mlo_roc_release = 3;
EXPORT_SYMBOL_GPL(mt7925_mlo_roc_release);
module_param_named(mlo_roc_release, mt7925_mlo_roc_release, byte, 0644);
MODULE_PARM_DESC(mlo_roc_release,
		 "bitmask: 1=per-link abort in roc_iter, 2=release JOIN slots of removed links at teardown (survivor slot kept), 8=legacy release-all (default 3)");

/* Hand the driver 802.11 frames instead of 802.3 encap-offload frames on
 * MT7927. The mlo_tx_pn_owner_live_link split in mt7925e only handles native
 * DATA; this is the same effect as mt792x_lib's test_mt7927_no_tx_encap but
 * lives in mt7925_common so the shared core (and the MT7921 stack holding it)
 * need not be reloaded. Read once at wiphy init, so it needs a reload.
 */
static bool mt7925_mlo_native_tx;
module_param_named(mlo_native_tx, mt7925_mlo_native_tx, bool, 0444);
MODULE_PARM_DESC(mlo_native_tx,
		 "TEST-ONLY: clear SUPPORTS_TX_ENCAP_OFFLOAD on MT7927 so data is native 802.11 (default N)");

/* Low-rate MLO control-plane markers (ROC request/grant/abort, pair
 * transition states, continuity guard, key/STA lifecycle). Independent of
 * the per-packet mlo_diag_trace so transitions can be observed under load.
 */
bool mt7925_mlo_ctl_trace;
EXPORT_SYMBOL_GPL(mt7925_mlo_ctl_trace);
module_param_named(mlo_ctl_trace, mt7925_mlo_ctl_trace, bool, 0644);
MODULE_PARM_DESC(mlo_ctl_trace,
		 "print low-rate MLO control-plane markers without enabling per-packet mlo_diag_trace");

static ssize_t mt7925_thermal_temp_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	switch (to_sensor_dev_attr(attr)->index) {
	case 0: {
		struct mt792x_phy *phy = dev_get_drvdata(dev);
		struct mt792x_dev *mdev = phy->dev;
		int temperature;

		mt792x_mutex_acquire(mdev);
		temperature = mt7925_mcu_get_temperature(phy);
		mt792x_mutex_release(mdev);

		if (temperature < 0)
			return temperature;
		/* display in millidegree Celsius */
		return sprintf(buf, "%u\n", temperature * 1000);
	}
	default:
		return -EINVAL;
	}
}
static SENSOR_DEVICE_ATTR_RO(temp1_input, mt7925_thermal_temp, 0);

static struct attribute *mt7925_hwmon_attrs[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt7925_hwmon);

static int mt7925_thermal_init(struct mt792x_phy *phy)
{
	struct wiphy *wiphy = phy->mt76->hw->wiphy;
	struct device *hwmon;
	const char *name;

	if (!IS_REACHABLE(CONFIG_HWMON))
		return 0;

	name = devm_kasprintf(&wiphy->dev, GFP_KERNEL, "mt7925_%s",
			      wiphy_name(wiphy));
	if (!name)
		return -ENOMEM;

	hwmon = devm_hwmon_device_register_with_groups(&wiphy->dev, name, phy,
						       mt7925_hwmon_groups);
	return PTR_ERR_OR_ZERO(hwmon);
}

static void mt7925_mac_init_basic_rates(struct mt792x_dev *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt76_rates); i++) {
		u16 rate = mt76_rates[i].hw_value;
		u16 idx = MT792x_BASIC_RATES_TBL + i;

		rate = FIELD_PREP(MT_TX_RATE_MODE, rate >> 8) |
		       FIELD_PREP(MT_TX_RATE_IDX, rate & GENMASK(7, 0));
		mt7925_mac_set_fixed_rate_table(dev, idx, rate);
	}
}

int mt7925_mac_init(struct mt792x_dev *dev)
{
	int i;

	mt76_rmw_field(dev, MT_MDP_DCR1, MT_MDP_DCR1_MAX_RX_LEN, 1536);
	/* enable hardware de-agg */
	mt76_set(dev, MT_MDP_DCR0, MT_MDP_DCR0_DAMSDU_EN);

	for (i = 0; i < MT792x_WTBL_SIZE; i++)
		mt7925_mac_wtbl_update(dev, i,
				       MT_WTBL_UPDATE_ADM_COUNT_CLEAR);
	for (i = 0; i < 2; i++)
		mt792x_mac_init_band(dev, i);

	mt7925_mac_init_basic_rates(dev);

	memzero_explicit(&dev->mt76.alpha2, sizeof(dev->mt76.alpha2));

	return 0;
}
EXPORT_SYMBOL_GPL(mt7925_mac_init);

static int __mt7925_init_hardware(struct mt792x_dev *dev)
{
	int ret;

	ret = mt792x_mcu_init(dev);
	if (ret)
		goto out;

	ret = mt76_eeprom_override(&dev->mphy);
	if (ret)
		goto out;

	ret = mt7925_mcu_set_eeprom(dev);
	if (ret)
		goto out;

	ret = mt7925_mac_init(dev);
	if (ret)
		goto out;

	if (is_mt7927(&dev->mt76) && mt7925_mt7927_dbdc) {
		ret = mt7925_mcu_set_dbdc(&dev->mphy, true);
		if (ret) {
			dev_warn(dev->mt76.dev,
				 "MT7927 DBDC enable failed: %d\n", ret);
			ret = 0;
		}
	}

out:
	return ret;
}

static int mt7925_init_hardware(struct mt792x_dev *dev)
{
	int ret, i;

	set_bit(MT76_STATE_INITIALIZED, &dev->mphy.state);

	for (i = 0; i < MT792x_MCU_INIT_RETRY_COUNT; i++) {
		if (atomic_read(&dev->mt76.bus_hung)) {
			ret = -EIO;
			break;
		}

		ret = __mt7925_init_hardware(dev);
		if (!ret)
			break;

		if (atomic_read(&dev->mt76.bus_hung))
			break;

		mt792x_init_reset(dev);
	}

	if (i == MT792x_MCU_INIT_RETRY_COUNT) {
		dev_err(dev->mt76.dev, "hardware init failed\n");
		return ret;
	}

	return 0;
}

/* compat: the cfg80211 NAN API arrived in kernel 7.2 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
static int mt7925_init_nan_cap(struct mt76_dev *mdev)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	const struct ieee80211_sta_he_cap *he_cap;
	struct ieee80211_supported_band *sband;
	struct wiphy *wiphy = mdev->hw->wiphy;

	if (!(dev->fw_features & MT792x_FW_CAP_NAN))
		return 0;

	sband = wiphy->bands[NL80211_BAND_2GHZ];
	if (sband)
		wiphy->nan_capa.phy.ht = sband->ht_cap;

	sband = wiphy->bands[NL80211_BAND_5GHZ];
	if (sband)
		wiphy->nan_capa.phy.vht = sband->vht_cap;

	sband = wiphy->bands[NL80211_BAND_2GHZ];
	he_cap = sband ? ieee80211_get_he_iftype_cap(sband, NL80211_IFTYPE_NAN)
		       : NULL;
	if (he_cap)
		wiphy->nan_capa.phy.he = *he_cap;

	return 0;
}
#endif

static void mt7925_init_work(struct work_struct *work)
{
	struct mt792x_dev *dev = container_of(work, struct mt792x_dev,
					      init_work);
	int ret;

	ret = mt7925_init_hardware(dev);
	if (ret)
		return;

	mt76_set_stream_caps(&dev->mphy, true);
	mt7925_set_stream_he_eht_caps(&dev->phy);
	mt792x_config_mac_addr_list(dev);

	ret = mt7925_init_mlo_caps(&dev->phy);
	if (ret) {
		dev_err(dev->mt76.dev, "MLO init failed\n");
		return;
	}

/* compat: the cfg80211 NAN API arrived in kernel 7.2 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
	dev->mt76.init_wiphy = mt7925_init_nan_cap;
#endif

	ret = mt76_register_device(&dev->mt76, true, mt76_rates,
				   ARRAY_SIZE(mt76_rates));
	if (ret) {
		dev_err(dev->mt76.dev, "register device failed\n");
		return;
	}

	ret = mt7925_init_debugfs(dev);
	if (ret) {
		dev_err(dev->mt76.dev, "register debugfs failed\n");
		return;
	}

	ret = mt7925_thermal_init(&dev->phy);
	if (ret) {
		dev_err(dev->mt76.dev, "thermal init failed\n");
		return;
	}

	ret = mt7925_mcu_set_thermal_protect(dev);
	if (ret) {
		dev_err(dev->mt76.dev, "thermal protection enable failed\n");
		return;
	}

	/* we support chip reset now */
	dev->hw_init_done = true;

	mt7925_mcu_set_deep_sleep(dev, dev->pm.ds_enable);
}

int mt7925_register_device(struct mt792x_dev *dev)
{
	struct ieee80211_hw *hw = mt76_hw(dev);
	int ret;

	dev->phy.dev = dev;
	dev->phy.mt76 = &dev->mt76.phy;
	dev->mt76.phy.priv = &dev->phy;
	dev->mt76.tx_worker.fn = mt792x_tx_worker;

	INIT_DELAYED_WORK(&dev->pm.ps_work, mt792x_pm_power_save_work);
	INIT_DELAYED_WORK(&dev->mlo_pm_work, mt7925_mlo_pm_work);
	INIT_WORK(&dev->pm.wake_work, mt792x_pm_wake_work);
	spin_lock_init(&dev->pm.wake.lock);
	mutex_init(&dev->pm.mutex);
	init_waitqueue_head(&dev->pm.wait);
	init_waitqueue_head(&dev->wait);
	spin_lock_init(&dev->pm.txq_lock);
	INIT_DELAYED_WORK(&dev->mphy.mac_work, mt792x_mac_work);
	INIT_DELAYED_WORK(&dev->phy.scan_work, mt7925_scan_work);
	INIT_DELAYED_WORK(&dev->coredump.work, mt7925_coredump_work);
#if IS_ENABLED(CONFIG_IPV6)
	INIT_WORK(&dev->ipv6_ns_work, mt7925_set_ipv6_ns_work);
	skb_queue_head_init(&dev->ipv6_ns_list);
#endif
	skb_queue_head_init(&dev->phy.scan_event_list);
	skb_queue_head_init(&dev->coredump.msg_list);

	INIT_WORK(&dev->reset_work, mt7925_mac_reset_work);
	INIT_WORK(&dev->init_work, mt7925_init_work);
/* compat: the cfg80211 NAN API arrived in kernel 7.2 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
	INIT_WORK(&dev->nan_deferred_work, mt7925_nan_deferred_work);
	spin_lock_init(&dev->nan_deferred_lock);
#endif

	INIT_WORK(&dev->phy.roc_work, mt7925_roc_work);
	timer_setup(&dev->phy.roc_timer, mt792x_roc_timer, 0);
	init_waitqueue_head(&dev->phy.roc_wait);

	dev->pm.idle_timeout = MT792x_PM_TIMEOUT;
	dev->pm.stats.last_wake_event = jiffies;
	dev->pm.stats.last_doze_event = jiffies;
	/* MT7927: runtime PM crashes BT firmware on the shared CONNINFRA domain */
	if (!mt76_is_usb(&dev->mt76) && !is_mt7927(&dev->mt76)) {
		dev->pm.enable_user = true;
		dev->pm.enable = true;
		dev->pm.ds_enable_user = true;
		dev->pm.ds_enable = true;
	}

	if (!mt76_is_mmio(&dev->mt76))
		hw->extra_tx_headroom += MT_SDIO_TXD_SIZE + MT_SDIO_HDR_SIZE;

	mt792x_init_acpi_sar(dev);

	ret = mt792x_init_wcid(dev);
	if (ret)
		return ret;

	ret = mt792x_init_wiphy(hw);
	if (ret)
		return ret;
	if (is_mt7927(&dev->mt76))
		hw->vif_data_size = sizeof(struct mt7925_mlo_vif);

	if (is_mt7927(&dev->mt76) && mt7925_mlo_native_tx)
		__clear_bit(IEEE80211_HW_SUPPORTS_TX_ENCAP_OFFLOAD, hw->flags);

	hw->wiphy->reg_notifier = mt7925_regd_notifier;
	dev->mphy.sband_2g.sband.ht_cap.cap |=
			IEEE80211_HT_CAP_LDPC_CODING |
			IEEE80211_HT_CAP_MAX_AMSDU;
	dev->mphy.sband_2g.sband.ht_cap.ampdu_density =
			IEEE80211_HT_MPDU_DENSITY_2;
	dev->mphy.sband_5g.sband.ht_cap.cap |=
			IEEE80211_HT_CAP_LDPC_CODING |
			IEEE80211_HT_CAP_MAX_AMSDU;
	dev->mphy.sband_2g.sband.ht_cap.ampdu_density =
			IEEE80211_HT_MPDU_DENSITY_1;
	dev->mphy.sband_5g.sband.vht_cap.cap |=
			IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454 |
			IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK |
			IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE |
			IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE |
			(3 << IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT);
	dev->mphy.sband_5g.sband.vht_cap.cap |=
			IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160MHZ |
			IEEE80211_VHT_CAP_SHORT_GI_160;

	dev->mphy.hw->wiphy->available_antennas_rx = dev->mphy.chainmask;
	dev->mphy.hw->wiphy->available_antennas_tx = dev->mphy.chainmask;

	queue_work(system_wq, &dev->init_work);

	return 0;
}
EXPORT_SYMBOL_GPL(mt7925_register_device);
