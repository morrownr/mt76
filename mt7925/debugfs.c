// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2023 MediaTek Inc. */

#include "mt7925.h"
#include "mlo/pairs.h"
#include "mcu.h"
#include "../mt76_connac.h"
#include <linux/capability.h>

static int
mt7925_reg_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	u32 regval = val;

	mt792x_mutex_acquire(dev);
	mt7925_mcu_regval(dev, dev->mt76.debugfs_reg, &regval, true);
	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_reg_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;
	u32 regval;
	int ret;

	mt792x_mutex_acquire(dev);
	ret = mt7925_mcu_regval(dev, dev->mt76.debugfs_reg, &regval, false);
	mt792x_mutex_release(dev);
	if (!ret)
		*val = regval;

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_regval, mt7925_reg_get, mt7925_reg_set,
			 "0x%08llx\n");
static int
mt7925_fw_debug_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;

	mt792x_mutex_acquire(dev);

	dev->fw_debug = (u8)val;
	mt7925_mcu_fw_log_2_host(dev, dev->fw_debug);

	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_fw_debug_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->fw_debug;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_fw_debug, mt7925_fw_debug_get,
			 mt7925_fw_debug_set, "%lld\n");

DEFINE_SHOW_ATTRIBUTE(mt792x_tx_stats);

static void
mt7925_seq_puts_array(struct seq_file *file, const char *str,
		      s8 val[][2], int len, u8 band_idx)
{
	int i;

	seq_printf(file, "%-22s:", str);
	for (i = 0; i < len; i++)
		if (val[i][band_idx] == 127)
			seq_printf(file, " %6s", "N.A");
		else
			seq_printf(file, " %6d", val[i][band_idx]);
	seq_puts(file, "\n");
}

#define mt7925_print_txpwr_entry(prefix, rate, idx)	\
({							\
	mt7925_seq_puts_array(s, #prefix " (tmac)",	\
			      txpwr->rate,		\
			      ARRAY_SIZE(txpwr->rate),	\
			      idx);			\
})

static inline void
mt7925_eht_txpwr(struct seq_file *s, struct mt7925_txpwr *txpwr, u8 band_idx)
{
	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "mcs0", "mcs1", "mcs2", "mcs3", "mcs4", "mcs5",
		   "mcs6", "mcs7", "mcs8", "mcs9", "mcs10", "mcs11",
		   "mcs12", "mcs13", "mcs14", "mcs15");
	mt7925_print_txpwr_entry(EHT26, eht26, band_idx);
	mt7925_print_txpwr_entry(EHT52, eht52, band_idx);
	mt7925_print_txpwr_entry(EHT106, eht106, band_idx);
	mt7925_print_txpwr_entry(EHT242, eht242, band_idx);
	mt7925_print_txpwr_entry(EHT484, eht484, band_idx);

	mt7925_print_txpwr_entry(EHT996, eht996, band_idx);
	mt7925_print_txpwr_entry(EHT996x2, eht996x2, band_idx);
	mt7925_print_txpwr_entry(EHT996x4, eht996x4, band_idx);
	mt7925_print_txpwr_entry(EHT26_52, eht26_52, band_idx);
	mt7925_print_txpwr_entry(EHT26_106, eht26_106, band_idx);
	mt7925_print_txpwr_entry(EHT484_242, eht484_242, band_idx);
	mt7925_print_txpwr_entry(EHT996_484, eht996_484, band_idx);
	mt7925_print_txpwr_entry(EHT996_484_242, eht996_484_242, band_idx);
	mt7925_print_txpwr_entry(EHT996x2_484, eht996x2_484, band_idx);
	mt7925_print_txpwr_entry(EHT996x3, eht996x3, band_idx);
	mt7925_print_txpwr_entry(EHT996x3_484, eht996x3_484, band_idx);
}

static int
mt7925_txpwr(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	struct mt7925_txpwr *txpwr = NULL;
	u8 band_idx = dev->mphy.band_idx;
	int ret = 0;

	txpwr = devm_kmalloc(dev->mt76.dev, sizeof(*txpwr), GFP_KERNEL);

	if (!txpwr)
		return -ENOMEM;

	mt792x_mutex_acquire(dev);
	ret = mt7925_get_txpwr_info(dev, band_idx, txpwr);
	mt792x_mutex_release(dev);

	if (ret)
		goto out;

	seq_printf(s, "%-22s  %6s %6s %6s %6s\n",
		   " ", "1m", "2m", "5m", "11m");
	mt7925_print_txpwr_entry(CCK, cck, band_idx);

	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "6m", "9m", "12m", "18m", "24m", "36m",
		   "48m", "54m");
	mt7925_print_txpwr_entry(OFDM, ofdm, band_idx);

	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "mcs0", "mcs1", "mcs2", "mcs3", "mcs4", "mcs5",
		   "mcs6", "mcs7");
	mt7925_print_txpwr_entry(HT20, ht20, band_idx);

	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "mcs0", "mcs1", "mcs2", "mcs3", "mcs4", "mcs5",
		   "mcs6", "mcs7", "mcs32");
	mt7925_print_txpwr_entry(HT40, ht40, band_idx);

	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "mcs0", "mcs1", "mcs2", "mcs3", "mcs4", "mcs5",
		   "mcs6", "mcs7", "mcs8", "mcs9", "mcs10", "mcs11");
	mt7925_print_txpwr_entry(VHT20, vht20, band_idx);
	mt7925_print_txpwr_entry(VHT40, vht40, band_idx);

	mt7925_print_txpwr_entry(VHT80, vht80, band_idx);
	mt7925_print_txpwr_entry(VHT160, vht160, band_idx);

	mt7925_print_txpwr_entry(HE26, he26, band_idx);
	mt7925_print_txpwr_entry(HE52, he52, band_idx);
	mt7925_print_txpwr_entry(HE106, he106, band_idx);
	mt7925_print_txpwr_entry(HE242, he242, band_idx);
	mt7925_print_txpwr_entry(HE484, he484, band_idx);

	mt7925_print_txpwr_entry(HE996, he996, band_idx);
	mt7925_print_txpwr_entry(HE996x2, he996x2, band_idx);

	mt7925_eht_txpwr(s, txpwr, band_idx);

out:
	devm_kfree(dev->mt76.dev, txpwr);
	return ret;
}

static int
mt7925_pm_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	struct mt76_connac_pm *pm = &dev->pm;

	if (mt76_is_usb(&dev->mt76))
		return -EOPNOTSUPP;

	mutex_lock(&dev->mt76.mutex);

	if (val == pm->enable_user)
		goto out;

	if (!pm->enable_user) {
		pm->stats.last_wake_event = jiffies;
		pm->stats.last_doze_event = jiffies;
	}
	/* make sure the chip is awake here and ps_work is scheduled
	 * just at end of the this routine.
	 */
	pm->enable = false;
	mt76_connac_pm_wake(&dev->mphy, pm);

	pm->enable_user = val;
	mt7925_set_runtime_pm(dev);
	mt76_connac_power_save_sched(&dev->mphy, pm);
out:
	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

static int
mt7925_pm_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->pm.enable_user;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_pm, mt7925_pm_get, mt7925_pm_set, "%lld\n");

static int
mt7925_deep_sleep_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	struct mt76_connac_pm *pm = &dev->pm;
	bool monitor = !!(dev->mphy.hw->conf.flags & IEEE80211_CONF_MONITOR);
	bool enable = !!val;

	if (mt76_is_usb(&dev->mt76))
		return -EOPNOTSUPP;

	mt792x_mutex_acquire(dev);
	if (pm->ds_enable_user == enable)
		goto out;

	pm->ds_enable_user = enable;
	pm->ds_enable = enable && !monitor;
	mt7925_mcu_set_deep_sleep(dev, pm->ds_enable);
out:
	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_deep_sleep_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->pm.ds_enable_user;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_ds, mt7925_deep_sleep_get,
			 mt7925_deep_sleep_set, "%lld\n");

DEFINE_DEBUGFS_ATTRIBUTE(fops_pm_idle_timeout, mt792x_pm_idle_timeout_get,
			 mt792x_pm_idle_timeout_set, "%lld\n");

static int mt7925_chip_reset(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	int ret = 0;

	switch (val) {
	case 1:
		/* Reset wifisys directly. */
		mt792x_reset(&dev->mt76);
		break;
	default:
		/* Collect the core dump before reset wifisys. */
		mt792x_mutex_acquire(dev);
		ret = mt7925_mcu_chip_config(dev, "assert");
		mt792x_mutex_release(dev);
		break;
	}

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_reset, NULL, mt7925_chip_reset, "%lld\n");

/* TEST-ONLY: per-MLO-link traffic counters, one row per WCID. */
static int mt7925_link_stats_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	int i;

	seq_printf(s, "%4s %4s %4s %6s %14s %10s %14s %10s %10s\n",
		   "wcid", "link", "phy", "valid",
		   "tx_bytes", "tx_pkts", "rx_bytes", "rx_pkts", "rate_kbps");
	seq_puts(s, "# rate_info: bw enum (0=20MHz 3=40 4=80 5=160 7=320), mcs, nss\n");

	mt792x_mutex_acquire(dev);
	for (i = 0; i < ARRAY_SIZE(dev->mt76.wcid); i++) {
		struct mt76_wcid *wcid = rcu_dereference_protected(
			dev->mt76.wcid[i], lockdep_is_held(&dev->mt76.mutex));

		if (!wcid)
			continue;
		seq_printf(s, "%4d %4d %4d %6d %14llu %10u %14llu %10u %10u  bw=%u mcs=%u nss=%u\n",
			   wcid->idx, wcid->link_id, wcid->phy_idx,
			   wcid->link_valid,
			   wcid->stats.tx_bytes, wcid->stats.tx_packets,
			   wcid->stats.rx_bytes, wcid->stats.rx_packets,
			   cfg80211_calculate_bitrate(&wcid->rate) * 100,
			   wcid->rate.bw, wcid->rate.mcs, wcid->rate.nss);
		if (wcid->sta) {
			struct mt792x_link_sta *ls =
				container_of(wcid, struct mt792x_link_sta, wcid);
			struct ieee80211_sta *sta = ls->sta ?
				container_of((void *)ls->sta, struct ieee80211_sta,
					     drv_priv) : NULL;
			struct mt76_txq *tid0_mtxq =
				(sta && sta->txq[0]) ?
				(struct mt76_txq *)sta->txq[0]->drv_priv : NULL;

			struct mt792x_bss_conf *mc =
				ls->sta ? mt792x_vif_to_link(ls->sta->vif,
							     wcid->link_id) : NULL;

			int wmhz = (mc && mc->mt76.ctx) ?
				cfg80211_chandef_get_width(&mc->mt76.ctx->def) : -1;
			u32 ac_tx = ls->airtime_ac[0] + ls->airtime_ac[1] +
				    ls->airtime_ac[2] + ls->airtime_ac[3];

			seq_printf(s, "       width_mhz=%d cum_tx_airtime_us=%u delta_us=%u elapsed_us=%u raw_util=%u%% util_ema=%u%% spare_weight=%u normalized_weight=%u bucket_share=%u/1024 new_flow_placements=%llu flow_cache_hits=%llu\n",
				   wmhz, ac_tx,
				   mt7925_mlo_dbg_delta(wcid->link_id),
				   mt7925_mlo_dbg_elapsed(wcid->link_id),
				   mt7925_mlo_dbg_raw_util(wcid->link_id),
				   mt7925_mlo_dbg_util(wcid->link_id),
				   mt7925_mlo_dbg_spare(wcid->link_id),
				   mt7925_mlo_dbg_weight(wcid->link_id),
				   mt7925_mlo_dbg_weight(wcid->link_id),
				   mt7925_mlo_dbg_placements(wcid->link_id),
				   mt7925_mlo_dbg_cache_hits(wcid->link_id));
			seq_printf(s, "       diag hw_key_idx=%u hw_key_idx2=%u cipher=%u ampdu_state=0x%lx tid0_aggr=%u tid0_ssn=%u bss_idx=%d omac_idx=%d wmm_idx=%d band_idx=%d chan_freq=%d\n",
				   wcid->hw_key_idx, wcid->hw_key_idx2,
				   wcid->cipher, wcid->ampdu_state,
				   tid0_mtxq ? tid0_mtxq->aggr : 0,
				   tid0_mtxq ? tid0_mtxq->agg_ssn : 0,
				   mc ? mc->mt76.idx : -1,
				   mc ? mc->mt76.omac_idx : -1,
				   mc ? mc->mt76.wmm_idx : -1,
				   mc ? mc->mt76.band_idx : -1,
				   (mc && mc->mt76.ctx && mc->mt76.ctx->def.chan) ?
				   mc->mt76.ctx->def.chan->center_freq : -1);
			seq_printf(s, "       airtime tx_ac=%u/%u/%u/%u rx_ac=%u/%u/%u/%u\n",
				   ls->airtime_ac[0], ls->airtime_ac[1],
				   ls->airtime_ac[2], ls->airtime_ac[3],
				   ls->airtime_ac[4], ls->airtime_ac[5],
				   ls->airtime_ac[6], ls->airtime_ac[7]);
			seq_printf(s, "       mlo valid_rx=%u last_valid_rx_ms_ago=%ld ready_ms=%ld await_valid_rx=%u await_gen=%u\n",
				   ls->mlo_valid_rx_packets,
				   ls->mlo_valid_rx_jiffies ?
				   (long)jiffies_to_msecs(jiffies - ls->mlo_valid_rx_jiffies) : -1L,
				   (ls->sta && wcid->link_id < IEEE80211_MLD_MAX_NUM_LINKS &&
				    ls->sta->mlo_link_ready_jiffies[wcid->link_id]) ?
				   (long)jiffies_to_msecs(jiffies -
					ls->sta->mlo_link_ready_jiffies[wcid->link_id]) : -1L,
				   ls->mlo_await_valid_rx, ls->mlo_await_generation);
		}
	}
	mt792x_mutex_release(dev);

	return 0;
}

/* TEST-ONLY: per-WCID key/PN/aggregation state, to compare link0 vs link2
 * before/after forcing bulk TX onto a link.
 */
static int mt7925_mlo_wcid_dump_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	int i, tid;

	seq_printf(s, "%4s %4s %6s %8s %9s %6s %6s %10s %6s %s\n",
		   "wcid", "link", "valid", "cipher", "hw_key_idx", "idx2",
		   "sw_iv", "tx_info", "sta", "aggr[tid]=state");

	mt792x_mutex_acquire(dev);
	for (i = 0; i < ARRAY_SIZE(dev->mt76.wcid); i++) {
		struct mt76_wcid *wcid = rcu_dereference_protected(
			dev->mt76.wcid[i], lockdep_is_held(&dev->mt76.mutex));

		if (!wcid)
			continue;
		seq_printf(s, "%4d %4d %6d %8u %9u %6u %6u %10u %6u ",
			   wcid->idx, wcid->link_id, wcid->link_valid,
			   wcid->cipher, wcid->hw_key_idx, wcid->hw_key_idx2,
			   wcid->sw_iv, wcid->tx_info, wcid->sta);
		for (tid = 0; tid < IEEE80211_NUM_TIDS; tid++) {
			struct mt76_rx_tid *aggr = rcu_dereference_protected(
				wcid->aggr[tid],
				lockdep_is_held(&dev->mt76.mutex));
			seq_printf(s, "%d=%s ", tid, aggr ? "set" : "-");
		}
		seq_printf(s, "ampdu_state=0x%lx rx_check_pn=%u\n",
			   wcid->ampdu_state, wcid->rx_check_pn);
	}
	mt792x_mutex_release(dev);

	return 0;
}

/* Read-only identity for DUT-filtered probes, not association/key/firmware proof.
 * Do not expose another device's pointers or introduce a shared runtime knob.
 */
static int
mt7925_mlo_capture_context_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!dev || !dev->mt76.dev || !dev->mt76.hw ||
	    !is_mt7927(&dev->mt76) ||
	    strcmp(dev_name(dev->mt76.dev), "0000:07:00.0"))
		return -ENODEV;

	seq_printf(s, "{\"version\":1,\"bdf\":\"%s\",\"phy\":\"%s\","
		   "\"mt792x_dev\":\"%px\",\"mt76_dev\":\"%px\",\"hw\":\"%px\"}\n",
		   dev_name(dev->mt76.dev), wiphy_name(dev->mt76.hw->wiphy),
		   dev, &dev->mt76, dev->mt76.hw);
	return 0;
}

struct mt7925_mlo_str_cap_ctx {
	struct seq_file *s;
	bool found;
};

static void mt7925_mlo_str_cap_iter(void *priv, u8 *mac,
				    struct ieee80211_vif *vif)
{
	struct mt7925_mlo_str_cap_ctx *ctx = priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct wiphy *wiphy = mvif->phy->mt76->hw->wiphy;
	const struct wiphy_iftype_ext_capab *ext_capa;

	if (vif->type != NL80211_IFTYPE_STATION || !ieee80211_vif_is_mld(vif))
		return;
	if (ctx->found)
		return;
	ctx->found = true;

	seq_puts(ctx->s, "MLO capability:\n");
	seq_puts(ctx->s, "  STR:\n");
	seq_printf(ctx->s, "    byte0=0x%02x\n", mvif->mlo_adv_str_cap[0]);
	seq_printf(ctx->s, "    byte1=0x%02x\n", mvif->mlo_adv_str_cap[1]);
	seq_printf(ctx->s, "    byte2=0x%02x\n", mvif->mlo_adv_str_cap[2]);
	seq_puts(ctx->s, "  EML:\n");
	seq_printf(ctx->s, "    byte0=0x%02x\n", mvif->mlo_adv_eml_cap[0]);
	seq_printf(ctx->s, "    byte1=0x%02x\n", mvif->mlo_adv_eml_cap[1]);
	seq_puts(ctx->s, "  mode:\n");
	seq_printf(ctx->s, "    %s\n",
		  mvif->mlo_adv_is_str ? "STR-advertised" : "EMLSR-advertised");

	ext_capa = cfg80211_get_iftype_ext_capa(wiphy,
						ieee80211_vif_type_p2p(vif));
	seq_puts(ctx->s, "  MLD capability/operations (live, wiphy_iftype_ext_capab):\n");
	if (ext_capa)
		seq_printf(ctx->s, "    mld_capa_and_ops=0x%04x eml_capabilities=0x%04x\n",
			  ext_capa->mld_capa_and_ops, ext_capa->eml_capabilities);
	else
		seq_puts(ctx->s, "    no-ext-capa\n");
}

static int mt7925_mlo_str_cap_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	struct mt7925_mlo_str_cap_ctx ctx = { .s = s, .found = false };

	ieee80211_iterate_active_interfaces(mt76_hw(dev),
					    IEEE80211_IFACE_ITER_RESUME_ALL,
					    mt7925_mlo_str_cap_iter, &ctx);
	if (!ctx.found)
		seq_puts(s, "no associated MLD station vif\n");

	return 0;
}

/* EXPERIMENTAL: resume link activation held by mlo_activate_hold. Runs as a
 * wiphy work item so it has the same (wiphy-locked) context as the AUTHORIZE
 * event it replaces.
 */
struct mt7925_act_now {
	struct wiphy_work work;
	struct mt792x_dev *dev;
	int ret;
};

static void mt7925_act_now_iter(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt7925_act_now *a = priv;

	if (vif->type == NL80211_IFTYPE_STATION && ieee80211_vif_is_mld(vif) &&
	    mt7925_mlo_activate_resume(a->dev, vif) == 0)
		a->ret = 0;
}

static void mt7925_act_now_fn(struct wiphy *wiphy, struct wiphy_work *w)
{
	struct mt7925_act_now *a = container_of(w, struct mt7925_act_now, work);

	a->ret = -ENOENT;
	ieee80211_iterate_active_interfaces_mtx(mt76_hw(a->dev),
						IEEE80211_IFACE_ITER_NORMAL,
						mt7925_act_now_iter, a);
}

static ssize_t mt7925_mlo_activate_now_write(struct file *file,
					     const char __user *ubuf,
					     size_t count, loff_t *ppos)
{
	struct mt792x_dev *dev = file->private_data;
	struct mt7925_act_now a = { .dev = dev };

	wiphy_work_init(&a.work, mt7925_act_now_fn);
	wiphy_work_queue(mt76_hw(dev)->wiphy, &a.work);
	wiphy_work_flush(mt76_hw(dev)->wiphy, &a.work);

	return a.ret ? a.ret : count;
}

static const struct file_operations fops_mlo_activate_now = {
	.write = mt7925_mlo_activate_now_write,
	.open = simple_open,
	.llseek = default_llseek,
};

/* EXPERIMENT: re-send BSS_INFO (basic + RLM channel/bandwidth TLVs) for every
 * active link of the MLD station vif. Used to test whether the 20 MHz
 * bandwidth the MLO JOIN request carries is left in force after the grant.
 */
struct mt7925_bss_resend {
	struct wiphy_work work;
	struct mt792x_dev *dev;
	int n;
};

static void mt7925_bss_resend_iter(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt7925_bss_resend *r = priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long act = vif->active_links;
	unsigned int id;

	if (vif->type != NL80211_IFTYPE_STATION || !ieee80211_vif_is_mld(vif))
		return;

	mt792x_mutex_acquire(r->dev);
	for_each_set_bit(id, &act, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_bss_conf *lc = mt792x_vif_to_bss_conf(vif, id);
		struct mt792x_bss_conf *mc = mt792x_vif_to_link(mvif, id);

		if (!lc || !mc || !mc->mt76.ctx)
			continue;
		pr_info("MLO_BSS_RESEND: link_id=%u ret=%d\n", id,
			mt7925_mcu_add_bss_info(&r->dev->phy, mc->mt76.ctx, lc,
						NULL, true));
		r->n++;
	}
	mt792x_mutex_release(r->dev);
}

static void mt7925_bss_resend_fn(struct wiphy *wiphy, struct wiphy_work *w)
{
	struct mt7925_bss_resend *r = container_of(w, struct mt7925_bss_resend, work);

	ieee80211_iterate_active_interfaces_mtx(mt76_hw(r->dev),
						IEEE80211_IFACE_ITER_NORMAL,
						mt7925_bss_resend_iter, r);
}

static ssize_t mt7925_mlo_bss_resend_write(struct file *file,
					   const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	struct mt792x_dev *dev = file->private_data;
	struct mt7925_bss_resend r = { .dev = dev };

	wiphy_work_init(&r.work, mt7925_bss_resend_fn);
	wiphy_work_queue(mt76_hw(dev)->wiphy, &r.work);
	wiphy_work_flush(mt76_hw(dev)->wiphy, &r.work);

	return r.n ? count : -ENOENT;
}

static const struct file_operations fops_mlo_bss_resend = {
	.write = mt7925_mlo_bss_resend_write,
	.open = simple_open,
	.llseek = default_llseek,
};

/* TEST-ONLY: force a specific active-link bitmap, e.g.
 *   echo 1 > mlo_active_links   -> only link 0
 *   echo 5 > mlo_active_links   -> links 0 and 2
 */
struct mt7925_set_links_ctx {
	struct mt792x_dev *dev;
	u16 links;
	struct ieee80211_vif *vif;
};

static void mt7925_set_links_iter(void *priv, u8 *mac,
				  struct ieee80211_vif *vif)
{
	struct mt7925_set_links_ctx *ctx = priv;
	u16 links = ctx->links;

	if (vif->type != NL80211_IFTYPE_STATION || !ieee80211_vif_is_mld(vif))
		return;

	pr_info("mt7925: forcing active_links 0x%x -> 0x%x (valid 0x%x dormant 0x%x)\n",
		vif->active_links, links, vif->valid_links, vif->dormant_links);
	/* Defer the request itself: ieee80211_request_neg_ttlm() takes the
	 * wiphy mutex, and this iterator runs under local->iflist_mtx.
	 */
	ctx->vif = vif;
}

static int mt7925_mlo_active_links_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	struct mt7925_set_links_ctx ctx = {
		.dev = dev,
		.links = val & 0xffff,
	};

	ieee80211_iterate_active_interfaces(mt76_hw(dev),
					    IEEE80211_IFACE_ITER_RESUME_ALL,
					    mt7925_set_links_iter, &ctx);
	if (!ctx.vif)
		return -ENODEV;
	return mt7925_mlo_request_active_links_negotiated(dev, ctx.vif, ctx.links);
}

static void mt7925_get_links_iter(void *priv, u8 *mac,
				  struct ieee80211_vif *vif)
{
	u64 *val = priv;

	if (vif->type != NL80211_IFTYPE_STATION || !ieee80211_vif_is_mld(vif))
		return;

	/* bits 15:0 = mac80211 active_links, bits 31:16 = valid_links */
	*val = vif->active_links | ((u64)vif->valid_links << 16);
}

static int mt7925_mlo_active_links_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = 0;
	ieee80211_iterate_active_interfaces(mt76_hw(dev),
					    IEEE80211_IFACE_ITER_RESUME_ALL,
					    mt7925_get_links_iter, val);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_mlo_active_links, mt7925_mlo_active_links_get,
			 mt7925_mlo_active_links_set, "0x%llx\n");

/* TEST-ONLY: live-mask the EML Capabilities value mac80211 advertises to
 * the AP in (re)association requests (wiphy->iftype_ext_capab[0]
 * .eml_capabilities, set once from firmware's NIC_CAP report at
 * mt7925_init_mlo_caps(), main.c). IEEE80211_EML_CAP_EMLSR_SUPP = BIT(0):
 * if firmware reports EMLSR support there, mac80211 advertises it over the
 * air regardless of the internal BSS_MLD eml_enable TLV
 * (test_mt7927_force_str_no_eml), and the AP is then free to schedule this
 * association as EMLSR (MU-RTS-gated, single listening radio per IEEE
 * 802.11be Figure AH-23) even while both radios are genuinely active
 * (STR). This node ANDs the given mask onto the live wiphy struct mac80211
 * already holds a pointer into, so it takes effect on the next
 * (re)association -- no module reload needed. Write 0xfffe to strip only
 * EMLSR_SUPP; write 0xffff to restore whatever firmware reported (undoes
 * a previous mask -- requires the raw value to have been read via
 * mlo_diag_trace's MLO_EML_CAP_ADVERTISE line first, this node cannot
 * restore it blindly).
 */
static int mt7925_mlo_eml_cap_mask_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	struct wiphy *wiphy = dev->mt76.hw->wiphy;
	struct wiphy_iftype_ext_capab *ec;

	if (!wiphy->iftype_ext_capab || !wiphy->num_iftype_ext_capab)
		return -ENODEV;

	ec = (struct wiphy_iftype_ext_capab *)&wiphy->iftype_ext_capab[0];
	pr_info("mt7925: mlo_eml_cap_mask 0x%04x -> 0x%04x (mask 0x%04llx)\n",
		ec->eml_capabilities, ec->eml_capabilities & (u16)val, val);
	ec->eml_capabilities &= (u16)val;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_mlo_eml_cap_mask, NULL,
			 mt7925_mlo_eml_cap_mask_set, "%llu\n");

/* TEST-ONLY: write a negative dBm value to replace Link 2 RSSI in the MLO
 * pair scorer. Writing 0 disables the override and restores live telemetry.
 */
static ssize_t mt7925_mlo_link2_rssi_override_read(struct file *file,
						   char __user *user_buf,
						   size_t count, loff_t *ppos)
{
	char buf[16];
	int len;

	len = scnprintf(buf, sizeof(buf), "%d\n",
			READ_ONCE(mt7925_mlo_link2_rssi_override));
	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static ssize_t mt7925_mlo_link2_rssi_override_write(struct file *file,
						    const char __user *user_buf,
						    size_t count, loff_t *ppos)
{
	int value, err;

	err = kstrtoint_from_user(user_buf, count, 0, &value);
	if (err)
		return err;
	if (value > 0 || value < S8_MIN)
		return -ERANGE;

	WRITE_ONCE(mt7925_mlo_link2_rssi_override, value);
	pr_info("mt7925: MLO Link 2 RSSI override %s value=%d dBm\n",
		value ? "enabled" : "disabled", value);

	return count;
}

/* TEST-ONLY diagnostic probe: write a link mask; for each active link of the
 * associated MLD peer, invalidate the unicast WCID's PTK marker and rerun
 * mt7925_mlo_reconcile_link() as the terminal firmware operation. Used to
 * test whether post-steering cm=1/icv_err=1 clears once the PTK is installed
 * after all STA_REC/STA_REC_MLD updates have settled.
 */
struct mt7925_ptk_probe_ctx {
	struct mt792x_dev *dev;
	unsigned long mask;
	bool starec;
	bool recreate;
	bool assoc_order;
};

static void mt7925_ptk_probe_iter(void *data, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt7925_ptk_probe_ctx *ctx = data;
	struct mt792x_dev *dev = ctx->dev;
	struct mt792x_sta *msta;
	struct ieee80211_sta *sta;
	unsigned long links;
	unsigned int link_id;

	if (vif->type != NL80211_IFTYPE_STATION || !ieee80211_vif_is_mld(vif) ||
	    !vif->cfg.assoc)
		return;

	rcu_read_lock();
	sta = ieee80211_find_sta(vif, vif->cfg.ap_addr);
	rcu_read_unlock();
	if (!sta)
		return;

	msta = (struct mt792x_sta *)sta->drv_priv;
	links = vif->active_links & ctx->mask;

	mt792x_mutex_acquire(dev);
	for_each_set_bit(link_id, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_link_sta *mlink = mt792x_sta_to_link(msta, link_id);
		int err;

		if (!mlink)
			continue;

		if (ctx->assoc_order) {
			struct ieee80211_link_sta *link_sta =
				mt792x_sta_to_link_sta(vif, sta, link_id);

			if (!link_sta)
				continue;
			err = mt7925_mcu_sta_update(dev, link_sta, vif, mlink, false,
						    MT76_STA_INFO_STATE_NONE, NULL);
			pr_info("MLO_ASSOC_ORDER_PROBE: stage=remove link=%u ret=%d\n", link_id, err);
			err = mt7925_mcu_sta_update(dev, link_sta, vif, mlink, true,
						    MT76_STA_INFO_STATE_NONE, NULL);
			pr_info("MLO_ASSOC_ORDER_PROBE: stage=create link=%u ret=%d\n", link_id, err);
			err = mt7925_mcu_sta_authorize(dev, link_sta, vif, mlink);
			pr_info("MLO_ASSOC_ORDER_PROBE: stage=authorize link=%u ret=%d\n", link_id, err);
			mlink->wcid.hw_key_idx = 255;
			mlink->wcid.hw_key_idx2 = 255;
			err = mt7925_mlo_reconcile_link(dev, vif, sta, mlink, link_id);
			pr_info("MLO_ASSOC_ORDER_PROBE: stage=ptk link=%u ret=%d hw_key_idx=%d\n",
				link_id, err, mlink->wcid.hw_key_idx);
			continue;
		}

		if (ctx->starec || ctx->recreate) {
			struct ieee80211_link_sta *link_sta =
				mt792x_sta_to_link_sta(vif, sta, link_id);

			if (!link_sta)
				continue;
			if (ctx->recreate) {
				err = mt7925_mcu_sta_rebind_phy(dev, link_sta,
								vif, mlink);
				pr_info("MLO_RECREATE_PROBE: stage=create link=%u wcid=%u ret=%d\n",
					link_id, mlink->wcid.idx, err);
				mlink->wcid.hw_key_idx = 255;
				err = mt7925_mlo_reconcile_link(dev, vif, sta,
								mlink, link_id);
				pr_info("MLO_RECREATE_PROBE: stage=restore link=%u ret=%d\n",
					link_id, err);
			}
			err = mt7925_mcu_sta_authorize(dev, link_sta, vif, mlink);
			pr_info("MLO_STAREC_PROBE: link=%u wcid=%u ret=%d\n",
				link_id, mlink->wcid.idx, err);
			continue;
		}

		pr_info("MLO_PTK_PROBE: stage=before link=%u wcid=%u hw_key_idx=%d\n",
			link_id, mlink->wcid.idx, mlink->wcid.hw_key_idx);
		mlink->wcid.hw_key_idx = 255;
		err = mt7925_mlo_reconcile_link(dev, vif, sta, mlink, link_id);
		pr_info("MLO_PTK_PROBE: stage=after link=%u wcid=%u hw_key_idx=%d ret=%d\n",
			link_id, mlink->wcid.idx, mlink->wcid.hw_key_idx, err);
	}
	mt792x_mutex_release(dev);
}

static ssize_t mt7925_ptk_probe_write(struct file *file,
				      const char __user *user_buf,
				      size_t count, loff_t *ppos)
{
	struct mt792x_dev *dev = file->private_data;
	struct ieee80211_hw *hw = mt76_hw(dev);
	struct mt7925_ptk_probe_ctx ctx = { .dev = dev };
	unsigned int mask;
	int err;

	err = kstrtouint_from_user(user_buf, count, 0, &mask);
	if (err)
		return err;

	ctx.mask = mask & 0xffff;
	ctx.starec = !!(mask & 0x10000);
	ctx.recreate = !!(mask & 0x20000);
	ctx.assoc_order = !!(mask & 0x40000);
	wiphy_lock(hw->wiphy);
	ieee80211_iterate_active_interfaces_mtx(hw, 0, mt7925_ptk_probe_iter,
						&ctx);
	wiphy_unlock(hw->wiphy);

	return count;
}

static const struct file_operations fops_mlo_ptk_probe = {
	.owner = THIS_MODULE,
	.write = mt7925_ptk_probe_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static const struct file_operations fops_mlo_link2_rssi_override = {
	.owner = THIS_MODULE,
	.read = mt7925_mlo_link2_rssi_override_read,
	.write = mt7925_mlo_link2_rssi_override_write,
	.llseek = default_llseek,
};

/* EXPERIMENTAL: forces mt792x_tx() (mt792x_core.c) to route this STA's
 * outgoing data frames onto a specific MLO link's WCID, overriding whatever
 * link mac80211 itself selected. Internally -1 = off (default, mac80211's
 * own choice is used); the debugfs write path only accepts unsigned values
 * (DEFINE_DEBUGFS_ATTRIBUTE parses writes with kstrtoull regardless of the
 * format string, so "-1" cannot be written directly), so 255 on the wire
 * means "off" and is translated to -1 internally. See
 * mt7925_mlo_force_tx_link in mt792x_core.c for the actual TX-path logic.
 *
 *   echo 255 > mlo_force_tx_link  -> off, back to mac80211's own selection
 *   echo   0 > mlo_force_tx_link  -> force link 0
 *   echo   1 > mlo_force_tx_link  -> force link 1
 *   echo   2 > mlo_force_tx_link  -> force link 2
 *   echo   3 > mlo_force_tx_link  -> EXPERIMENTAL: alternate link 0/2 on
 *                                    successive bulk-TXQ scheduling decisions
 *   cat mlo_force_tx_link         -> read current override (255 = off)
 */
extern int mt7925_mlo_force_tx_link;

#define MT7925_MLO_FORCE_ALTERNATE (-2)
#define MT7925_MLO_FORCE_FLOWHASH (-3)
#define MT7925_MLO_FORCE_CAPHASH (-4)
#define MT7925_MLO_FORCE_LOADAWARE (-5)

static int mt7925_mlo_force_tx_link_set(void *data, u64 val)
{
	if (val == 3)
		mt7925_mlo_force_tx_link = MT7925_MLO_FORCE_ALTERNATE;
	else if (val == 4)
		mt7925_mlo_force_tx_link = MT7925_MLO_FORCE_FLOWHASH;
	else if (val == 5)
		mt7925_mlo_force_tx_link = MT7925_MLO_FORCE_CAPHASH;
	else if (val == 6)
		mt7925_mlo_force_tx_link = MT7925_MLO_FORCE_LOADAWARE;
	else if (val > 15)
		mt7925_mlo_force_tx_link = -1;
	else
		mt7925_mlo_force_tx_link = (int)val;
	return 0;
}

static int mt7925_mlo_force_tx_link_get(void *data, u64 *val)
{
	if (mt7925_mlo_force_tx_link == MT7925_MLO_FORCE_ALTERNATE)
		*val = 3;
	else if (mt7925_mlo_force_tx_link == MT7925_MLO_FORCE_FLOWHASH)
		*val = 4;
	else if (mt7925_mlo_force_tx_link == MT7925_MLO_FORCE_CAPHASH)
		*val = 5;
	else if (mt7925_mlo_force_tx_link == MT7925_MLO_FORCE_LOADAWARE)
		*val = 6;
	else
		*val = mt7925_mlo_force_tx_link < 0 ? 255 : (u64)mt7925_mlo_force_tx_link;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_mlo_force_tx_link, mt7925_mlo_force_tx_link_get,
			 mt7925_mlo_force_tx_link_set, "%llu\n");

/* TEST-ONLY, READ-ONLY: live WM firmware RAM dumper for offline firmware
 * reverse engineering (MLSR / channel-privilege / radio-slot audit).
 *
 * Whitelist-only. Reads go through the SAME accessor every other register
 * read in this driver uses (mt76_rr() -> mt7925_rr() ->
 * __mt7925_reg_addr()'s fixed_map/L1/L2 remap in pci.c), under the same
 * dev->mt76.mutex + PM-wake that mt792x_mutex_acquire() everywhere else
 * uses. No firmware command is issued and no runtime state is written;
 * this is a plain MMIO read through the existing remap windows. There is
 * no arbitrary-address interface: only the six fixed WM download
 * destinations below are exposed, one read-only debugfs file each.
 *
 * Large regions are streamed in small chunks (MT7925_FW_RAM_CHUNK_WORDS
 * words = 1 KiB), each under its own short mutex acquisition, so a full
 * region dump does not hold dev->mt76.mutex continuously and stall other
 * mac80211/driver work for the whole transfer.
 */
struct mt7925_fw_ram_region {
	u32 dest;
	u32 len;
};

static const struct mt7925_fw_ram_region mt7925_fw_ram_regions[] = {
	{ 0x0091c000, 0x00003d10 }, /* region00 */
	{ 0x02212800, 0x00055fd0 }, /* region01 */
	{ 0x00404000, 0x00007fd0 }, /* region02 */
	{ 0xe0140000, 0x0004ffd0 }, /* region03 */
	{ 0xe006c000, 0x000a87d0 }, /* region04 */
	{ 0xe0114800, 0x0002b7d0 }, /* region05 */
};

#define MT7925_FW_RAM_NREGIONS ARRAY_SIZE(mt7925_fw_ram_regions)
#define MT7925_FW_RAM_CHUNK_WORDS 256 /* 1 KiB per mutex acquisition */

struct mt7925_fw_ram_ctx {
	struct mt792x_dev *dev;
	unsigned int region;
};

static ssize_t mt7925_fw_ram_read(struct file *file, char __user *user_buf,
				  size_t count, loff_t *ppos)
{
	struct mt7925_fw_ram_ctx *ctx = file->private_data;
	struct mt792x_dev *dev = ctx->dev;
	const struct mt7925_fw_ram_region *r = &mt7925_fw_ram_regions[ctx->region];
	loff_t pos = *ppos;
	size_t done = 0;

	if (pos < 0)
		return -EINVAL;
	if (pos >= r->len)
		return 0;
	if (count > (size_t)(r->len - pos))
		count = r->len - pos;

	while (done < count) {
		u32 byte_off = (u32)(pos + done);
		u32 word_off = byte_off & ~3U;
		u32 shift = byte_off & 3U;
		u32 want = (u32)min_t(size_t, count - done,
				      MT7925_FW_RAM_CHUNK_WORDS * 4 - shift);
		u32 nwords = DIV_ROUND_UP(want + shift, 4);
		u32 buf[MT7925_FW_RAM_CHUNK_WORDS];
		u8 *bytes = (u8 *)buf;
		bool all_ff = true;
		u32 i;

		mt792x_mutex_acquire(dev);
		for (i = 0; i < nwords; i++) {
			buf[i] = mt76_rr(dev, r->dest + word_off + i * 4);
			if (buf[i] != 0xffffffffU)
				all_ff = false;
		}
		mt792x_mutex_release(dev);

		if (all_ff && nwords >= 4) {
			dev_warn(dev->mt76.dev,
				"MT7925_FW_RAM: region%u off=0x%x all-0xff for %u words, aborting read (device unreachable?)\n",
				ctx->region, word_off, nwords);
			return done ? done : -EIO;
		}

		if (copy_to_user(user_buf + done, bytes + shift, want))
			return done ? done : -EFAULT;

		done += want;
	}

	*ppos = pos + done;
	return done;
}

static const struct file_operations mt7925_fw_ram_fops = {
	.open = simple_open,
	.read = mt7925_fw_ram_read,
	.llseek = default_llseek,
};

static void mt7925_fw_ram_debugfs_init(struct mt792x_dev *dev,
				       struct dentry *dir)
{
	static const char * const names[] = {
		"fw_region00", "fw_region01", "fw_region02",
		"fw_region03", "fw_region04", "fw_region05",
	};
	unsigned int i;

	BUILD_BUG_ON(ARRAY_SIZE(names) != MT7925_FW_RAM_NREGIONS);

	if (!is_mt7927(&dev->mt76))
		return;

	for (i = 0; i < MT7925_FW_RAM_NREGIONS; i++) {
		struct mt7925_fw_ram_ctx *ctx;

		ctx = devm_kzalloc(dev->mt76.dev, sizeof(*ctx), GFP_KERNEL);
		if (!ctx)
			return;
		ctx->dev = dev;
		ctx->region = i;
		debugfs_create_file(names[i], 0400, dir, ctx,
				    &mt7925_fw_ram_fops);
	}
}

/* TEST-ONLY L2-remap transaction tracer.
 *
 * Not a general L2 access tracer -- fires exactly once, on first read,
 * for a fixed, tiny, hardcoded set of targets chosen to diagnose the
 * repeated 0x00000032 seen on fw_region04. It manually replays, with
 * instrumentation, exactly what mt7925_reg_map_l1()/mt7925_reg_map_l2()
 * in pci.c do -- via the same dev->bus_ops raw accessor those functions
 * use -- and additionally reads back MT_HIF_REMAP_L2 after programming
 * it (production code never reads L2 back) and restores L1 directly
 * from a locally saved value (bypassing the production backup_l1/
 * backup_l2 path, which mt7925_reg_map_l2() does not populate
 * correctly -- see l2_analysis.md, Phase A finding on backup_l2).
 *
 * Targets traced:
 *   - baseline: raw read at MT_HIF_REMAP_BASE_L1 with L1 left untouched
 *   - the four mandated region04 words (0xe006c000/+4, 0xe006d000/+4)
 *   - two cross-region high addresses already in the six-region
 *     whitelist (region00 0x0091c000, region03 0xe0140000), to test
 *     whether 0x32 is specific to region04 or universal to the L2 path
 *   - region02's fixed-map target (0x00404000) forced through the L2
 *     mechanism instead of the normal fixed_map lookup -- the strongest
 *     available "known-good" control, since no production caller in
 *     this driver exercises mt7925_reg_map_l2() at all (see
 *     l2_analysis.md Phase B) and this physical target's fixed-map
 *     value is already known from the Phase 3 probe.
 */
static char *mt7925_l2_trace_buf;
static bool mt7925_l2_trace_ran;

static void mt7925_l2_trace_one(struct mt792x_dev *dev, char *buf,
				size_t bufsz, size_t *off,
				const char *label, u32 addr)
{
	u32 base = FIELD_GET(MT_HIF_REMAP_L1_BASE, MT_HIF_REMAP_BASE_L2);
	u32 l1_before, l1_intended, l1_after, l2_intended, l2_after;
	u32 l1_push, xaddr, data, l1_restored;

	mt792x_mutex_acquire(dev);

	l1_before = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	l1_intended = (l1_before & ~(u32)MT_HIF_REMAP_L1_MASK) |
		      FIELD_PREP(MT_HIF_REMAP_L1_MASK, base);
	dev->bus_ops->rmw(&dev->mt76, MT_HIF_REMAP_L1, MT_HIF_REMAP_L1_MASK,
			  FIELD_PREP(MT_HIF_REMAP_L1_MASK, base));
	l1_after = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	l2_intended = addr;
	dev->bus_ops->wr(&dev->mt76, MT_HIF_REMAP_L2, addr);
	/* extra readback -- production code never does this */
	l2_after = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L2);

	/* "use read to push write", exactly as production does */
	l1_push = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	xaddr = MT_HIF_REMAP_BASE_L1;
	data = dev->bus_ops->rr(&dev->mt76, xaddr);

	/* test-only immediate restore, not via the buggy backup path */
	dev->bus_ops->wr(&dev->mt76, MT_HIF_REMAP_L1, l1_before);
	l1_restored = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	mt792x_mutex_release(dev);

	*off += scnprintf(buf + *off, bufsz - *off,
		"---- %s ----\n"
		"target_addr           = 0x%08x\n"
		"L1 before              = 0x%08x\n"
		"L1 intended             = 0x%08x\n"
		"L1 readback(after prog) = 0x%08x\n"
		"L2 intended             = 0x%08x\n"
		"L2 readback(after prog) = 0x%08x\n"
		"L1 readback(push read)  = 0x%08x\n"
		"translated_addr         = 0x%08x\n"
		"data read at translated = 0x%08x\n"
		"L1 restored/readback    = 0x%08x\n\n",
		label, addr, l1_before, l1_intended, l1_after,
		l2_intended, l2_after, l1_push, xaddr, data, l1_restored);
}

/* Same MT_HIF_REMAP_L1 register, but tests a NON-magic base value (the
 * base mt7925_reg_map_l1() itself would compute for a real L1-range
 * address), to discriminate "L1 write is universally stuck" from
 * "only the L2-indirect magic bank 0x1850 is blocked". No production
 * MT7927 code path currently exercises mt7925_reg_map_l1() either (see
 * l2_analysis.md Phase B) -- mt7927_wfsys_reset() uses only passthrough
 * (<0x200000) registers -- so this is also a from-scratch probe, not a
 * verified-working reference, but it isolates the variable under test.
 */
static void mt7925_l1_trace_one(struct mt792x_dev *dev, char *buf,
				size_t bufsz, size_t *off,
				const char *label, u32 addr)
{
	u32 offset = FIELD_GET(MT_HIF_REMAP_L1_OFFSET, addr);
	u32 base = FIELD_GET(MT_HIF_REMAP_L1_BASE, addr);
	u32 l1_before, l1_intended, l1_after, l1_push, xaddr, data, l1_restored;

	mt792x_mutex_acquire(dev);

	l1_before = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);
	l1_intended = (l1_before & ~(u32)MT_HIF_REMAP_L1_MASK) |
		      FIELD_PREP(MT_HIF_REMAP_L1_MASK, base);
	dev->bus_ops->rmw(&dev->mt76, MT_HIF_REMAP_L1, MT_HIF_REMAP_L1_MASK,
			  FIELD_PREP(MT_HIF_REMAP_L1_MASK, base));
	l1_after = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);
	l1_push = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	xaddr = MT_HIF_REMAP_BASE_L1 + offset;
	data = dev->bus_ops->rr(&dev->mt76, xaddr);

	dev->bus_ops->wr(&dev->mt76, MT_HIF_REMAP_L1, l1_before);
	l1_restored = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	mt792x_mutex_release(dev);

	*off += scnprintf(buf + *off, bufsz - *off,
		"---- %s ----\n"
		"target_addr             = 0x%08x (base=0x%04x offset=0x%04x)\n"
		"L1 before               = 0x%08x\n"
		"L1 intended              = 0x%08x\n"
		"L1 readback(after prog)  = 0x%08x\n"
		"L1 readback(push read)   = 0x%08x\n"
		"translated_addr          = 0x%08x\n"
		"data read at translated  = 0x%08x\n"
		"L1 restored/readback     = 0x%08x\n\n",
		label, addr, base, offset, l1_before, l1_intended, l1_after,
		l1_push, xaddr, data, l1_restored);
}

static void mt7925_l2_trace_baseline(struct mt792x_dev *dev, char *buf,
				     size_t bufsz, size_t *off)
{
	u32 l1_cur, data;

	mt792x_mutex_acquire(dev);
	l1_cur = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);
	data = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_BASE_L1);
	mt792x_mutex_release(dev);

	*off += scnprintf(buf + *off, bufsz - *off,
		"---- baseline (L1 left untouched this call) ----\n"
		"L1 current (untouched)  = 0x%08x\n"
		"raw read at 0x%08x  = 0x%08x\n\n",
		l1_cur, MT_HIF_REMAP_BASE_L1, data);
}

#define MT7925_L2_TRACE_BUFSZ 8192

static ssize_t mt7925_l2_trace_read(struct file *file, char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct mt792x_dev *dev = file->private_data;
	size_t off = 0;

	if (!is_mt7927(&dev->mt76))
		return -ENODEV;

	if (!mt7925_l2_trace_buf) {
		mt7925_l2_trace_buf = kzalloc(MT7925_L2_TRACE_BUFSZ,
					      GFP_KERNEL);
		if (!mt7925_l2_trace_buf)
			return -ENOMEM;
	}

	if (!mt7925_l2_trace_ran) {
		mt7925_l2_trace_baseline(dev, mt7925_l2_trace_buf,
					 MT7925_L2_TRACE_BUFSZ, &off);
		mt7925_l2_trace_one(dev, mt7925_l2_trace_buf,
				    MT7925_L2_TRACE_BUFSZ, &off,
				    "region04 word0  (0xe006c000)",
				    0xe006c000);
		mt7925_l2_trace_one(dev, mt7925_l2_trace_buf,
				    MT7925_L2_TRACE_BUFSZ, &off,
				    "region04 word1  (0xe006c004)",
				    0xe006c004);
		mt7925_l2_trace_one(dev, mt7925_l2_trace_buf,
				    MT7925_L2_TRACE_BUFSZ, &off,
				    "region04 +0x1000 word0 (0xe006d000)",
				    0xe006d000);
		mt7925_l2_trace_one(dev, mt7925_l2_trace_buf,
				    MT7925_L2_TRACE_BUFSZ, &off,
				    "region04 +0x1000 word1 (0xe006d004)",
				    0xe006d004);
		mt7925_l2_trace_one(dev, mt7925_l2_trace_buf,
				    MT7925_L2_TRACE_BUFSZ, &off,
				    "CROSS-REGION region00 word0 (0x0091c000)",
				    0x0091c000);
		mt7925_l2_trace_one(dev, mt7925_l2_trace_buf,
				    MT7925_L2_TRACE_BUFSZ, &off,
				    "CROSS-REGION region03 word0 (0xe0140000)",
				    0xe0140000);
		mt7925_l2_trace_one(dev, mt7925_l2_trace_buf,
				    MT7925_L2_TRACE_BUFSZ, &off,
				    "CONTROL: region02 target (0x00404000) FORCED via L2 -- known fixed-map value: 0x0000000c 0x00000010 0x00020002 ...",
				    0x00404000);
		mt7925_l1_trace_one(dev, mt7925_l2_trace_buf,
				   MT7925_L2_TRACE_BUFSZ, &off,
				   "L1-ONLY CONTROL: non-magic base, real WFSYS_SW_RST_B-style target (0x7c000140, base=0x7c00 != 0x1850)",
				   0x7c000140);
		mt7925_l2_trace_ran = true;
	}

	return simple_read_from_buffer(user_buf, count, ppos,
				       mt7925_l2_trace_buf,
				       strlen(mt7925_l2_trace_buf));
}

static const struct file_operations mt7925_l2_trace_fops = {
	.open = simple_open,
	.read = mt7925_l2_trace_read,
	.llseek = default_llseek,
};

int mt7925_init_debugfs(struct mt792x_dev *dev)
{
	struct dentry *dir;

	dir = mt76_register_debugfs_fops(&dev->mphy, &fops_regval);

	if (mt76_is_mmio(&dev->mt76))
		debugfs_create_devm_seqfile(dev->mt76.dev, "xmit-queues",
					    dir, mt792x_queues_read);
	else
		debugfs_create_devm_seqfile(dev->mt76.dev, "xmit-queues",
					    dir, mt76_queues_read);

	debugfs_create_devm_seqfile(dev->mt76.dev, "acq", dir,
				    mt792x_queues_acq);
	debugfs_create_devm_seqfile(dev->mt76.dev, "txpower_sku", dir,
				    mt7925_txpwr);
	debugfs_create_file("tx_stats", 0400, dir, dev, &mt792x_tx_stats_fops);
	debugfs_create_devm_seqfile(dev->mt76.dev, "link_stats", dir,
				    mt7925_link_stats_read);
	debugfs_create_devm_seqfile(dev->mt76.dev, "mlo_wcid_dump", dir,
				    mt7925_mlo_wcid_dump_read);
	if (is_mt7927(&dev->mt76) &&
	    !strcmp(dev_name(dev->mt76.dev), "0000:07:00.0"))
		debugfs_create_devm_seqfile(dev->mt76.dev, "mlo_capture_context", dir,
					    mt7925_mlo_capture_context_read);
	debugfs_create_devm_seqfile(dev->mt76.dev, "mlo_str_cap", dir,
				    mt7925_mlo_str_cap_read);
	debugfs_create_file("mlo_activate_now", 0200, dir, dev, &fops_mlo_activate_now);
	debugfs_create_file("mlo_bss_resend", 0200, dir, dev, &fops_mlo_bss_resend);
	debugfs_create_file("mlo_active_links", 0600, dir, dev,
			    &fops_mlo_active_links);
	debugfs_create_file("mlo_eml_cap_mask", 0200, dir, dev,
			    &fops_mlo_eml_cap_mask);
	debugfs_create_file("mlo_link2_rssi_override", 0600, dir, dev,
			    &fops_mlo_link2_rssi_override);
	debugfs_create_file("mlo_force_tx_link", 0600, dir, dev,
			    &fops_mlo_force_tx_link);
	debugfs_create_file("mlo_ptk_probe", 0200, dir, dev,
			    &fops_mlo_ptk_probe);
	debugfs_create_bool("mlo_diag_trace", 0600, dir,
			    &mt76_mlo_diag_trace);
	debugfs_create_u8("mlo_bssadd_skip", 0600, dir,
			  &mt76_mlo_bssadd_skip);
	debugfs_create_u32("mlo_bssadd_delay_ms", 0600, dir,
			   &mt76_mlo_bssadd_delay_ms);
	debugfs_create_file("fw_debug", 0600, dir, dev, &fops_fw_debug);
	debugfs_create_file("runtime-pm", 0600, dir, dev, &fops_pm);
	debugfs_create_file("idle-timeout", 0600, dir, dev,
			    &fops_pm_idle_timeout);
	debugfs_create_file("chip_reset", 0600, dir, dev, &fops_reset);
	debugfs_create_devm_seqfile(dev->mt76.dev, "runtime_pm_stats", dir,
				    mt792x_pm_stats);
	debugfs_create_file("deep-sleep", 0600, dir, dev, &fops_ds);

	mt7925_fw_ram_debugfs_init(dev, dir);

	if (is_mt7927(&dev->mt76))
		debugfs_create_file("l2_trace", 0400, dir, dev,
				    &mt7925_l2_trace_fops);

	return mt7927_mlo_pairs_debugfs_init(dev, dir);
}
