// SPDX-License-Identifier: GPL-2.0-only
/*
 * Core activation characterization, linked into mac80211.o by the offline
 * overlay runner. No device is registered and no hardware transport is used.
 * The core, driver-ops wrappers, channel/key operations and cfg80211 work are
 * real. ieee80211_ops below are explicit boundary doubles, NOT mt7925_ops.
 *
 * Mutations caught: move GTK switching across a STA callback, bypass the
 * intermediate survivor, notify an equal-mask STA operation, publish success
 * before a grow returns, or fail to restore the mask on a grow error.
 */
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/etherdevice.h>
#include <linux/kthread.h>
#include "../ieee80211_i.h"
#include "../key.h"
/* mt76 and mac80211 separately declare this EWMA type. Namespace only the
 * mt76 declaration in this test TU; layout/algorithm remain unchanged. */
#define ewma_signal mt76_kunit_ewma_signal
#define ewma_signal_init mt76_kunit_ewma_signal_init
#define ewma_signal_read mt76_kunit_ewma_signal_read
#define ewma_signal_add mt76_kunit_ewma_signal_add
#define ewma_avg_signal mt76_kunit_ewma_avg_signal
#define ewma_avg_signal_init mt76_kunit_ewma_avg_signal_init
#define ewma_avg_signal_read mt76_kunit_ewma_avg_signal_read
#define ewma_avg_signal_add mt76_kunit_ewma_avg_signal_add
#include "mt7925.h"
#include "mlo/pairs.h"
#undef ewma_avg_signal_add
#undef ewma_avg_signal_read
#undef ewma_avg_signal_init
#undef ewma_avg_signal
#undef ewma_signal_add
#undef ewma_signal_read
#undef ewma_signal_init
#undef ewma_signal

/* No mt76 production object is linked into this UML fixture. */
bool mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
/* This fixture links real core/completion/validator code, not pair debugfs.
 * Observation-only sinks have no firmware or completion side effects. Pair
 * profile/config behavior is exercised by test_mlo_device_isolation.py.
 */
void mt7927_mlo_pairs_note(struct ieee80211_vif *vif, u16 target,
			   int error, const char *reason, bool core_done) { }
void mt7927_mlo_pairs_invalidate(struct ieee80211_vif *vif, bool security) { }
void mlo_kunit_outer_result(struct ieee80211_vif *vif, int ret);
void mlo_kunit_sta_callsite(struct ieee80211_vif *vif, u16 old, u16 new);

/* Overlay-only seam into the genuine static interface initializers. */
void ieee80211_mlo_test_sdata_init(struct ieee80211_local *local,
				struct ieee80211_sub_if_data *sdata);

enum mlo_event_kind {
	MLO_VIF, MLO_STA, MLO_STA_CALLSITE, MLO_KEY_SET, MLO_KEY_DISABLE,
	MLO_ASSIGN, MLO_UNASSIGN,
};

struct mlo_event {
	enum mlo_event_kind kind;
	u16 old, new;
	int link;
};

struct mlo_fixture {
	struct ieee80211_hw *hw;
	struct ieee80211_local *local;
	struct ieee80211_sub_if_data *sdata;
	struct sta_info *sta;
	struct mt792x_dev *dev;
	struct net_device *netdev;
	struct ieee80211_channel channels[3];
	struct ieee80211_rate rate;
	struct ieee80211_supported_band band;
	struct mt792x_link_sta *peer[3];
	struct completion outer_done, grow_entered, grow_release;
	struct mlo_event events[64];
	spinlock_t log_lock;
	unsigned int n_events;
	int outer_ret;
	int fail_key_link;
	int fail_errno;
	u16 block_new, fail_new, refuse_new, fail_sta_new, fail_admission_new;
	bool log_overflow;
	bool sta_inserted;
	bool flows_initialized;
	bool completion_initialized;
	/* recovery-watch boundary double: what the completion owner asked for */
	unsigned int rec_armed, rec_cancelled;
	u16 rec_survivor, rec_failed, rec_prev, seeded_pair;
	u64 rec_epoch;
};

static const struct ieee80211_ops mlo_ops;

static struct mlo_fixture *mlo_fixture_from_vif(struct ieee80211_vif *vif)
{
	struct ieee80211_local *local = vif_to_sdata(vif)->local;

	if (local->ops != &mlo_ops)
		return NULL;
	return *(struct mlo_fixture **)local->hw.priv;
}

/* mlo/recovery.c is not linked here (it needs the mt792x reset worker). The
 * completion owner's calls into it are recorded so the arming rules are tested
 * against the real production owner; the watch itself is covered by the pure
 * policy test and the recovery contract.
 */
void mt7925_mlo_recovery_init(struct ieee80211_vif *vif) { }
void mt7925_mlo_recovery_stop(struct ieee80211_vif *vif) { }
void mt7925_mlo_recovery_arm_survivor(struct ieee80211_vif *vif, u16 survivors,
				      u16 failed, u16 prev, u64 epoch)
{
	struct mlo_fixture *f = mlo_fixture_from_vif(vif);

	if (!f)
		return;
	f->rec_armed++;
	f->rec_survivor = survivors;
	f->rec_failed = failed;
	f->rec_prev = prev;
	f->rec_epoch = epoch;
}
void mt7925_mlo_recovery_assoc_lost(struct ieee80211_vif *vif)
{
	struct mlo_fixture *f = mlo_fixture_from_vif(vif);

	if (f)
		f->rec_cancelled++;
}

static void mlo_record(struct mlo_fixture *f, enum mlo_event_kind kind,
		       u16 old, u16 new, int link)
{
	unsigned long flags;

	spin_lock_irqsave(&f->log_lock, flags);
	if (f->n_events < ARRAY_SIZE(f->events))
		f->events[f->n_events++] = (struct mlo_event) {
			.kind = kind, .old = old, .new = new, .link = link,
		};
	else
		f->log_overflow = true;
	spin_unlock_irqrestore(&f->log_lock, flags);
}

/* Called by the overlay only AFTER the real outer setter has returned. */
void mlo_kunit_outer_result(struct ieee80211_vif *vif, int ret)
{
	struct mlo_fixture *f = mlo_fixture_from_vif(vif);

	if (!f)
		return;
	WRITE_ONCE(f->outer_ret, ret);
	complete(&f->outer_done);
}

/* Called before masking in the real drv_change_sta_links wrapper. */
void mlo_kunit_sta_callsite(struct ieee80211_vif *vif, u16 old, u16 new)
{
	struct mlo_fixture *f = mlo_fixture_from_vif(vif);

	if (f)
		mlo_record(f, MLO_STA_CALLSITE, old, new, -1);
}

static int mlo_change_vif(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  u16 old, u16 new,
			  struct ieee80211_bss_conf *old_conf[IEEE80211_MLD_MAX_NUM_LINKS])
{
	struct mlo_fixture *f = mlo_fixture_from_vif(vif);

#ifdef MLO_KUNIT_COMPLETION_INCLUDE
	if (f->completion_initialized)
		mt7925_mlo_completion_changed(vif);
#endif
	mlo_record(f, MLO_VIF, old, new, -1);
	if (new == READ_ONCE(f->block_new) && (new & ~old)) {
		complete(&f->grow_entered);
		if (!wait_for_completion_timeout(&f->grow_release, 5 * HZ))
			return -ETIMEDOUT;
	}
	return new == READ_ONCE(f->fail_new) ?
		(f->fail_errno ? f->fail_errno : -EIO) : 0;
}

static int mlo_change_sta(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta, u16 old, u16 new)
{
	struct mlo_fixture *f = mlo_fixture_from_vif(vif);

	if (f->completion_initialized)
		mt7925_mlo_completion_changed(vif);
	mlo_record(f, MLO_STA, old, new, -1);
	if (new == f->fail_admission_new && (new & ~old)) {
		mt7925_mlo_completion_admission_failed(vif, old, new, f->seeded_pair, -ENODATA);
		mt7925_mlo_completion_note_error(vif, -ENODATA);
		return -ENODATA;
	}
	if (new == f->fail_sta_new) {
		mt7925_mlo_completion_note_error(vif, -EIO);
		return -EIO;
	}
	return 0;
}

static void mlo_idle_steer_work(struct work_struct *work) { }

static int mlo_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
		       struct ieee80211_vif *vif, struct ieee80211_sta *sta,
		       struct ieee80211_key_conf *key)
{
	struct mlo_fixture *f = mlo_fixture_from_vif(vif);

	/* Production mt7927_mlo_assoc_set_key() notifies the completion owner
	 * on every key callback, including the incoming-GTK switch that follows
	 * a failed replacement admission inside the same outer setter.
	 */
#ifdef MLO_KUNIT_COMPLETION_INCLUDE
	if (f->completion_initialized)
		mt7925_mlo_completion_key_changed(vif);
#endif
	mlo_record(f, cmd == SET_KEY ? MLO_KEY_SET :
		   MLO_KEY_DISABLE, 0, 0, key->link_id);
	if (cmd == SET_KEY && key->link_id == f->fail_key_link) {
#ifdef MLO_KUNIT_COMPLETION_INCLUDE
		mt7925_mlo_completion_note_error(vif, -EIO);
#endif
		return -EIO;
	}
	key->hw_key_idx = key->link_id;
	return 0;
}

static bool mlo_can_activate(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			     u16 new)
{
	return new != READ_ONCE(mlo_fixture_from_vif(vif)->refuse_new);
}

static int mlo_assign(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		      struct ieee80211_bss_conf *conf,
		      struct ieee80211_chanctx_conf *ctx)
{
	mlo_record(mlo_fixture_from_vif(vif), MLO_ASSIGN, 0, 0, conf->link_id);
	return 0;
}

static void mlo_unassign(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			 struct ieee80211_bss_conf *conf,
			 struct ieee80211_chanctx_conf *ctx)
{
	mlo_record(mlo_fixture_from_vif(vif), MLO_UNASSIGN, 0, 0, conf->link_id);
}

static void mlo_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *ctl,
		   struct sk_buff *skb) { dev_kfree_skb(skb); }
static int mlo_start(struct ieee80211_hw *hw) { return 0; }
static void mlo_stop(struct ieee80211_hw *hw, bool suspend) { }
static int mlo_config(struct ieee80211_hw *hw, int radio, u32 changed) { return 0; }
static int mlo_add_iface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{ return 0; }
static void mlo_remove_iface(struct ieee80211_hw *hw, struct ieee80211_vif *vif) { }
static void mlo_filter(struct ieee80211_hw *hw, unsigned int changed,
		       unsigned int *total, u64 multicast) { }
static void mlo_wake_txq(struct ieee80211_hw *hw, struct ieee80211_txq *txq) { }
static int mlo_add_ctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx)
{ return 0; }
static void mlo_remove_ctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx) { }
static void mlo_change_ctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx,
			   u32 changed) { }
static int mlo_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		      unsigned int link, u16 ac,
		      const struct ieee80211_tx_queue_params *params) { return 0; }
static int mlo_sta_state(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta, enum ieee80211_sta_state old,
			 enum ieee80211_sta_state new) { return 0; }

static const struct ieee80211_ops mlo_ops = {
	.tx = mlo_tx, .start = mlo_start, .stop = mlo_stop, .config = mlo_config,
	.add_interface = mlo_add_iface, .remove_interface = mlo_remove_iface,
	.configure_filter = mlo_filter, .wake_tx_queue = mlo_wake_txq,
	.add_chanctx = mlo_add_ctx, .remove_chanctx = mlo_remove_ctx,
	.change_chanctx = mlo_change_ctx, .assign_vif_chanctx = mlo_assign,
	.unassign_vif_chanctx = mlo_unassign, .conf_tx = mlo_conf_tx,
	.sta_state = mlo_sta_state, .change_vif_links = mlo_change_vif,
	.change_sta_links = mlo_change_sta, .set_key = mlo_set_key,
	.can_activate_links = mlo_can_activate,
};

static const struct ieee80211_iface_limit mlo_limit = {
	.max = 1, .types = BIT(NL80211_IFTYPE_STATION),
};
static const struct ieee80211_iface_combination mlo_combination = {
	.limits = &mlo_limit, .n_limits = 1, .max_interfaces = 1,
	.num_different_channels = 3,
};

static void mlo_reset_log(struct mlo_fixture *f)
{
	f->n_events = 0;
	f->log_overflow = false;
	f->outer_ret = -EINPROGRESS;
	reinit_completion(&f->outer_done);
	reinit_completion(&f->grow_entered);
	reinit_completion(&f->grow_release);
}

static int mlo_seed_active(struct mlo_fixture *f, u16 active)
{
	int ret;

	wiphy_lock(f->hw->wiphy);
	ret = ieee80211_set_active_links(&f->sdata->vif, active);
	wiphy_unlock(f->hw->wiphy);
	mlo_reset_log(f);
	/* The production caller reads the pair active before the transition from
	 * the transition record (pt_old_links); the fixture's equivalent is the seed.
	 */
	f->seeded_pair = active;
	return ret;
}

static void mlo_submit_async(struct mlo_fixture *f, u16 target)
{
	mlo_reset_log(f);
	wiphy_lock(f->hw->wiphy);
	ieee80211_set_active_links_async(&f->sdata->vif, target);
	wiphy_unlock(f->hw->wiphy);
}

static bool mlo_wait_outer(struct mlo_fixture *f)
{
	bool done = wait_for_completion_timeout(&f->outer_done, 6 * HZ);

	/* Never take the wiphy lock while a double is blocked holding it. */
	complete_all(&f->grow_release);
	wiphy_lock(f->hw->wiphy);
	wiphy_work_flush(f->hw->wiphy, &f->sdata->activate_links_work);
	wiphy_unlock(f->hw->wiphy);
	return done;
}

static int mlo_fixture_init(struct kunit *test);
static void mlo_fixture_exit(struct kunit *test);
#ifdef MLO_KUNIT_COMPLETION_INCLUDE
#include MLO_KUNIT_COMPLETION_INCLUDE
#endif

static void mlo_fixture_cleanup(struct mlo_fixture *f)
{
	if (!f->hw)
		return;
	complete_all(&f->grow_release);
	wiphy_lock(f->hw->wiphy);
	if (f->sdata) {
#ifdef MLO_KUNIT_COMPLETION_INCLUDE
		if (f->completion_initialized)
			mt7925_mlo_completion_assoc(&f->sdata->vif, false);
#endif
		wiphy_work_cancel(f->hw->wiphy, &f->sdata->activate_links_work);
		ieee80211_free_keys(f->sdata, false);
		if (f->sta_inserted) {
			int ret = __sta_info_destroy(f->sta);

			WARN_ON_ONCE(ret);
		}
		else if (f->sta)
			sta_info_free(f->local, f->sta);
		ieee80211_mgd_stop(f->sdata);
		ieee80211_vif_clear_links(f->sdata);
		ieee80211_link_stop(&f->sdata->deflink);
		wiphy_delayed_work_cancel(f->hw->wiphy,
					 &f->sdata->dec_tailroom_needed_wk);
		ieee80211_destroy_frag_cache(&f->sdata->frags);
		list_del(&f->sdata->list);
	}
	wiphy_unlock(f->hw->wiphy);
	if (f->local->workqueue)
		destroy_workqueue(f->local->workqueue);
	if (f->flows_initialized)
		ieee80211_txq_teardown_flows(f->local);
	if (f->netdev)
		free_netdev(f->netdev);
	synchronize_rcu();
	ieee80211_free_hw(f->hw);
	f->hw = NULL;
}

static int mlo_fixture_init(struct kunit *test)
{
	static const u8 ap[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0x20 };
	u8 address[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0x10 };
	struct mlo_fixture *f;
	struct mt792x_sta *msta;
	struct mt792x_vif *mvif;
	int i, ret = -ENOMEM;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	test->priv = f;
	f->fail_key_link = -1;
	spin_lock_init(&f->log_lock);
	init_completion(&f->outer_done);
	init_completion(&f->grow_entered);
	init_completion(&f->grow_release);
	f->hw = ieee80211_alloc_hw_nm(sizeof(f), &mlo_ops, NULL);
	if (!f->hw)
		return -ENOMEM;
	*(struct mlo_fixture **)f->hw->priv = f;
	f->local = hw_to_local(f->hw);
	f->hw->queues = 4;
	f->hw->vif_data_size = sizeof(struct mt7925_mlo_vif);
	f->hw->sta_data_size = sizeof(struct mt792x_sta);
	ieee80211_hw_set(f->hw, HAS_RATE_CONTROL);
	f->hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	f->hw->wiphy->iface_combinations = &mlo_combination;
	f->hw->wiphy->n_iface_combinations = 1;
	f->rate = (struct ieee80211_rate) { .bitrate = 60,
		.flags = IEEE80211_RATE_MANDATORY_A };
	for (i = 0; i < 3; i++)
		f->channels[i] = (struct ieee80211_channel) {
			.band = NL80211_BAND_5GHZ, .center_freq = 5180 + 20 * i,
			.hw_value = 36 + 4 * i, .max_power = 20,
		};
	f->band = (struct ieee80211_supported_band) {
		.band = NL80211_BAND_5GHZ, .channels = f->channels,
		.n_channels = 3, .bitrates = &f->rate, .n_bitrates = 1,
	};
	f->hw->wiphy->bands[NL80211_BAND_5GHZ] = &f->band;
	ret = ieee80211_txq_setup_flows(f->local);
	if (ret)
		goto fail;
	f->flows_initialized = true;
	f->dev = kunit_kzalloc(test, sizeof(*f->dev), GFP_KERNEL);
	if (!f->dev) {
		ret = -ENOMEM;
		goto fail;
	}
	mutex_init(&f->dev->mt76.mutex);
	f->dev->mt76.rev = 0x7927 << 16;
	f->dev->mt76.hw = f->hw;
	f->dev->mt76.phy.dev = &f->dev->mt76;
	f->dev->mt76.phy.hw = f->hw;
	f->dev->phy.dev = f->dev;
	f->dev->phy.mt76 = &f->dev->mt76.phy;
	f->dev->mt76.phy.priv = &f->dev->phy;
	ret = -ENOMEM;
	f->local->workqueue = alloc_ordered_workqueue("mlo-kunit", 0);
	if (!f->local->workqueue)
		goto fail;
	f->netdev = alloc_netdev(sizeof(*f->sdata) + f->hw->vif_data_size,
				"mlo-kunit", NET_NAME_UNKNOWN, ether_setup);
	if (!f->netdev)
		goto fail;
	f->sdata = netdev_priv(f->netdev);
	f->sdata->dev = f->netdev;
	strscpy(f->sdata->name, "mlo-kunit");
	wiphy_lock(f->hw->wiphy);
	ieee80211_mlo_test_sdata_init(f->local, f->sdata);
	f->netdev->ieee80211_ptr = &f->sdata->wdev;
	f->sdata->wdev.netdev = f->netdev;
	ether_addr_copy(f->sdata->vif.addr, address);
	ether_addr_copy(f->sdata->vif.cfg.ap_addr, ap);
	list_add_tail(&f->sdata->list, &f->local->interfaces);
	set_bit(SDATA_STATE_RUNNING, &f->sdata->state);
	f->sdata->flags |= IEEE80211_SDATA_IN_DRIVER;
	/* Synthetic association membership; constructors own actual links. */
	f->sdata->wdev.valid_links = 7;
	f->sdata->vif.active_links = 4;
	ret = ieee80211_vif_set_links(f->sdata, 7, 0);
	if (ret)
		goto unlock_fail;
	for (i = 0; i < 3; i++) {
		struct ieee80211_bss_conf *conf =
			wiphy_dereference(f->hw->wiphy, f->sdata->vif.link_conf[i]);

		cfg80211_chandef_create(&conf->chanreq.oper, &f->channels[i],
					NL80211_CHAN_NO_HT);
		conf->chanreq.ap = conf->chanreq.oper;
		ether_addr_copy(wiphy_dereference(f->hw->wiphy,
				f->sdata->link[i])->u.mgd.bssid, ap);
	}
	ret = _ieee80211_link_use_channel(
		wiphy_dereference(f->hw->wiphy, f->sdata->link[2]),
		&wiphy_dereference(f->hw->wiphy,
			f->sdata->vif.link_conf[2])->chanreq,
		IEEE80211_CHANCTX_SHARED, false);
	if (ret)
		goto unlock_fail;
	ret = ieee80211_set_active_links(&f->sdata->vif, 6);
	if (ret)
		goto unlock_fail;
	f->sta = sta_info_alloc_with_link(f->sdata, ap, 2, ap, GFP_KERNEL);
	if (!f->sta) {
		ret = -ENOMEM;
		goto unlock_fail;
	}
	f->sta->sta.mlo = true;
	ret = sta_info_insert(f->sta);
	if (ret) {
		f->sta = NULL; /* insertion frees on failure */
		goto unlock_fail;
	}
	f->sta_inserted = true;
	for (i = 0; i < 2; i++) {
		struct link_sta_info *link;

		ret = ieee80211_sta_allocate_link(f->sta, i);
		if (ret)
			goto unlock_fail;
		link = wiphy_dereference(f->hw->wiphy, f->sta->link[i]);
		address[5] = 0x30 + i;
		ether_addr_copy(link->addr, address);
		ether_addr_copy(link->pub->addr, address);
		ret = ieee80211_sta_activate_link(f->sta, i);
		if (ret)
			goto unlock_fail;
	}
	msta = (void *)f->sta->sta.drv_priv;
	mvif = (void *)f->sdata->vif.drv_priv;
	mvif->phy = &f->dev->phy;
	msta->vif = mvif;
	msta->valid_links = 7;
	msta->deflink_id = 2;
	mvif->wep_sta = msta;
	for (i = 0; i < 3; i++) {
		struct ieee80211_key *key;
		u8 material[16] = {};

		f->peer[i] = i == 2 ? &msta->deflink :
			kunit_kzalloc(test, sizeof(*f->peer[i]), GFP_KERNEL);
		if (!f->peer[i]) {
			ret = -ENOMEM;
			goto unlock_fail;
		}
		f->peer[i]->sta = msta;
		f->peer[i]->wcid.link_id = i;
		f->peer[i]->wcid.link_valid = true;
		f->peer[i]->wcid.hw_key_idx = 0;
		rcu_assign_pointer(msta->link[i], f->peer[i]);
		material[0] = i + 1;
		key = ieee80211_key_alloc(WLAN_CIPHER_SUITE_CCMP, 0,
					 sizeof(material), material, 0, NULL);
		if (IS_ERR(key)) {
			ret = PTR_ERR(key);
			goto unlock_fail;
		}
		key->conf.link_id = i;
		ret = ieee80211_key_link(key, wiphy_dereference(f->hw->wiphy,
					f->sdata->link[i]), NULL);
		if (ret)
			goto unlock_fail;
	}
	f->sdata->vif.cfg.assoc = true;
#ifdef MLO_KUNIT_COMPLETION_INCLUDE
	mt7925_mlo_completion_init(&f->sdata->vif);
	INIT_DELAYED_WORK(&((struct mt792x_vif *)f->sdata->vif.drv_priv)->mlo_steer_work,
			  mlo_idle_steer_work);
	f->completion_initialized = true;
	mt7925_mlo_completion_assoc(&f->sdata->vif, true);
#endif
	wiphy_unlock(f->hw->wiphy);
	mlo_reset_log(f);
	return 0;
unlock_fail:
	wiphy_unlock(f->hw->wiphy);
fail:
	mlo_fixture_cleanup(f);
	return ret;
}

static void mlo_fixture_exit(struct kunit *test)
{
	mlo_fixture_cleanup(test->priv);
}

static void mlo_callback_key_order_six_edges(struct kunit *test)
{
	/* Literal survivor, removed/added link and direction, hand-checked. */
	static const struct { u16 old, new, survivor; int remove, add; } edges[] = {
		{ 6, 5, 4, 1, 0 }, { 5, 3, 1, 2, 1 }, { 3, 6, 2, 0, 2 },
		{ 5, 6, 4, 0, 1 }, { 3, 5, 1, 1, 2 }, { 6, 3, 2, 2, 0 },
	};
	static const enum mlo_event_kind order[] = {
		MLO_UNASSIGN, MLO_KEY_DISABLE, MLO_STA, MLO_VIF,
		MLO_VIF, MLO_ASSIGN, MLO_STA, MLO_KEY_SET,
	};
	struct mlo_fixture *f = test->priv;
	int e;

	for (e = 0; e < ARRAY_SIZE(edges); e++) {
		struct mlo_event actual[8], calls[4];
		int i, n = 0, c = 0, ret;

		KUNIT_ASSERT_EQ(test, mlo_seed_active(f, edges[e].old), 0);
		wiphy_lock(f->hw->wiphy);
		ret = ieee80211_set_active_links(&f->sdata->vif, edges[e].new);
		wiphy_unlock(f->hw->wiphy);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, edges[e].new);
		KUNIT_EXPECT_FALSE(test, f->log_overflow);
		for (i = 0; i < f->n_events; i++) {
			if (f->events[i].kind == MLO_STA_CALLSITE) {
				if (c < ARRAY_SIZE(calls))
					calls[c] = f->events[i];
				c++;
			} else {
				if (n < ARRAY_SIZE(actual))
					actual[n] = f->events[i];
				n++;
			}
		}
		KUNIT_ASSERT_EQ(test, n, 8);
		KUNIT_ASSERT_EQ(test, c, 4);
		for (i = 0; i < ARRAY_SIZE(order); i++)
			KUNIT_EXPECT_EQ(test, actual[i].kind, order[i]);
		KUNIT_EXPECT_EQ(test, actual[1].link, edges[e].remove);
		KUNIT_EXPECT_EQ(test, actual[7].link, edges[e].add);
		KUNIT_EXPECT_EQ(test, actual[0].link, edges[e].remove);
		KUNIT_EXPECT_EQ(test, actual[5].link, edges[e].add);
		KUNIT_EXPECT_EQ(test, actual[3].old, edges[e].old);
		KUNIT_EXPECT_EQ(test, actual[3].new, edges[e].survivor);
		KUNIT_EXPECT_EQ(test, actual[4].old, edges[e].survivor);
		KUNIT_EXPECT_EQ(test, actual[4].new, edges[e].new);
		KUNIT_EXPECT_EQ(test, actual[2].old, edges[e].old);
		KUNIT_EXPECT_EQ(test, actual[2].new, edges[e].survivor);
		KUNIT_EXPECT_EQ(test, actual[6].old, edges[e].survivor);
		KUNIT_EXPECT_EQ(test, actual[6].new, edges[e].new);
		KUNIT_EXPECT_EQ(test, calls[0].old, edges[e].old);
		KUNIT_EXPECT_EQ(test, calls[0].new, edges[e].old);
		KUNIT_EXPECT_EQ(test, calls[1].old, edges[e].old);
		KUNIT_EXPECT_EQ(test, calls[1].new, edges[e].survivor);
		KUNIT_EXPECT_EQ(test, calls[2].old, edges[e].survivor);
		KUNIT_EXPECT_EQ(test, calls[2].new, edges[e].new);
		KUNIT_EXPECT_EQ(test, calls[3].old, edges[e].new);
		KUNIT_EXPECT_EQ(test, calls[3].new, edges[e].new);
	}
}

static void mlo_delayed_grow_mask_is_not_outer_completion(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	bool entered;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 4), 0);
	f->block_new = 5;
	mlo_submit_async(f, 5);
	entered = wait_for_completion_timeout(&f->grow_entered, 2 * HZ);
	KUNIT_EXPECT_TRUE(test, entered);
	if (entered) {
		KUNIT_EXPECT_EQ(test, READ_ONCE(f->sdata->vif.active_links), (u16)5);
		KUNIT_EXPECT_FALSE(test, completion_done(&f->outer_done));
		KUNIT_EXPECT_EQ(test, f->n_events, 1U); /* VIF, no STA/key/channel yet */
	}
	complete_all(&f->grow_release);
	KUNIT_EXPECT_TRUE(test, mlo_wait_outer(f));
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->outer_ret), 0);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)5);
}

static void mlo_refused_rollback_preserves_current_pair(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->refuse_new = 6;
	mlo_submit_async(f, 6);
	KUNIT_EXPECT_TRUE(test, mlo_wait_outer(f));
	KUNIT_EXPECT_EQ(test, f->outer_ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)5);
	KUNIT_EXPECT_EQ(test, f->n_events, 0U);
}

static void mlo_failed_rollback_transient_target_returns_to_survivor(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	bool entered;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->block_new = 6;
	f->fail_new = 6;
	mlo_submit_async(f, 6);
	entered = wait_for_completion_timeout(&f->grow_entered, 2 * HZ);
	KUNIT_EXPECT_TRUE(test, entered);
	if (entered) {
		KUNIT_EXPECT_EQ(test, READ_ONCE(f->sdata->vif.active_links), (u16)6);
		/* Declared input only; not RF/security validation or completion. */
		f->peer[2]->mlo_valid_rx_packets++;
		WRITE_ONCE(f->peer[2]->mlo_valid_rx_jiffies, jiffies);
		KUNIT_EXPECT_FALSE(test, completion_done(&f->outer_done));
	}
	complete_all(&f->grow_release);
	KUNIT_EXPECT_TRUE(test, mlo_wait_outer(f));
	KUNIT_EXPECT_EQ(test, f->outer_ret, -EIO);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)4);
	KUNIT_EXPECT_EQ(test, f->sdata->desired_active_links, (u16)0);
}

#ifdef MLO_KUNIT_VALIDATOR_INCLUDE
#include MLO_KUNIT_VALIDATOR_INCLUDE
#endif

static struct kunit_case mlo_activation_cases[] = {
	KUNIT_CASE(mlo_callback_key_order_six_edges),
	KUNIT_CASE(mlo_delayed_grow_mask_is_not_outer_completion),
	KUNIT_CASE(mlo_refused_rollback_preserves_current_pair),
	KUNIT_CASE(mlo_failed_rollback_transient_target_returns_to_survivor),
	{}
};

static struct kunit_suite mlo_activation_suite = {
	.name = "mlo_activation_core",
	.init = mlo_fixture_init,
	.exit = mlo_fixture_exit,
	.test_cases = mlo_activation_cases,
};
kunit_test_suite(mlo_activation_suite);
