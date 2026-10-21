// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Pair configuration and observations. Never programs a peer, key or radio. */
#include "pairs.h"
#include "mlo.h"
#include "../mlo_pair_transition.h"
#include <linux/seq_file.h>

int mt7927_mlo_pair_index(u16 mask)
{
	switch (mask) {
	case 0x3: return 0;
	case 0x5: return 1;
	case 0x6: return 2;
	default: return -EINVAL;
	}
}

bool mt7927_mlo_context_link_matches(unsigned int requested, int actual)
{
	return actual >= 0 && requested == (unsigned int)actual;
}

void mt7927_mlo_profiles_init(struct mt7927_mlo_profiles *p, bool mt7927)
{
	unsigned int i;

	memset(p, 0, sizeof(*p));
	p->device_supported = mt7927;
	p->interval_ms = 5000;
	for (i = 0; i < ARRAY_SIZE(p->pair); i++) {
		p->pair[i].mask = i == 0 ? 0x3 : i == 1 ? 0x5 : 0x6;
		p->pair[i].enabled = mt7927;
	}
	/* 5+6 GHz (0x6) is not a steering mode: both bands share firmware slot
	 * 1, so it is one radio time-shared and its second link has never
	 * carried authenticated traffic (Memory 943). Disabled by default so
	 * neither association nor steering lands in it; it stays selectable
	 * through debugfs for diagnostics.
	 */
	p->pair[2].enabled = false;
	/* Reference to this project's external historical record, NOT proof
	 * about whichever device/association loads this driver in the future.
	 */
	if (mt7927) {
		p->pair[1].historical_memory = 927;
		p->pair[0].prior_implementation_memory = 933;
		p->pair[1].prior_implementation_memory = 932;
		p->pair[2].prior_implementation_memory = 931;
	}
}

void mt7927_mlo_profiles_invalidate(struct mt7927_mlo_profiles *p)
{
	unsigned int i;

	p->observation_generation++;
	for (i = 0; i < ARRAY_SIZE(p->pair); i++)
		p->pair[i].current_validated = false;
}

bool mt7927_mlo_profiles_can_steer(const struct mt7927_mlo_profiles *p)
{
	unsigned int i, usable = 0;

	if (!p->device_supported || !p->authorized)
		return false;
	/* Steering chooses between the SIMULTANEOUS modes (2.4+5 = 0x3,
	 * 2.4+6 = 0x5 as link bitmaps). The single-radio 5+6 GHz bitmap
	 * (profile 2) is not a steering mode and no longer gates anything:
	 * 5 and 6 GHz share one RF slot and it never carried authenticated
	 * traffic on both links (Memory 943). A mode is proven by its own
	 * authenticated commit and guarded by validation, rollback and failure
	 * backoff, not by a stationary all-pairs pre-gate that nothing in the
	 * driver could ever satisfy. A mode known to be unsupported is out.
	 */
	for (i = 0; i < 2; i++)
		usable += p->pair[i].enabled &&
			  p->pair[i].support != MT7927_MLO_SUPPORT_UNSUPPORTED;
	return usable >= 2;
}

int mt7927_mlo_profiles_configure(struct mt7927_mlo_profiles *p,
				enum mt7927_mlo_control control, u64 value)
{
	if (!p->device_supported)
		return -ENODEV;
	if ((unsigned int)control <= MT7927_MLO_CTL_PAIR06) {
		if (value > 1)
			return -EINVAL;
		p->pair[control].enabled = value;
		return 0;
	}
	switch (control) {
	case MT7927_MLO_CTL_ENABLE:
		if (value > 1)
			return -EINVAL;
		return value && !mt7927_mlo_profiles_can_steer(p) ? -EAGAIN : 0;
	case MT7927_MLO_CTL_INTERVAL:
		if (value < 1000 || value > 600000)
			return -ERANGE;
		p->interval_ms = value;
		return 0;
	case MT7927_MLO_CTL_POLICY:
		/* Only the existing adaptive policy is migrated. A deterministic
		 * cycle is not implemented/certified by this offline refactor.
		 */
		if (value)
			return -EOPNOTSUPP;
		p->policy = value;
		return 0;
	case MT7927_MLO_CTL_FALLBACK:
		if (value && (value > 0xffff || mt7927_mlo_pair_index(value) < 0))
			return -EINVAL;
		/* 0 retains the existing ticketed previous-pair rollback. Do not
		 * advertise arbitrary fallback restoration before resource and
		 * data-path recovery have actually been established.
		 */
		if (value)
			return -EOPNOTSUPP;
		p->fallback_pair = 0;
		return 0;
	default:
		return -EINVAL;
	}
}

void mt7927_mlo_profile_note(struct mt7927_mlo_profiles *profiles, u16 target,
			     int error, const char *reason, u64 now, bool core_done)
{
	struct mt7927_mlo_pair_profile *p;
	int idx = mt7927_mlo_pair_index(target);

	if (idx < 0 || !profiles->device_supported)
		return;
	p = &profiles->pair[idx];
	if (!strcmp(reason, "request")) {
		p->last_attempt_ns = now;
		p->last_attempt_epoch = profiles->association_epoch;
	}
	if (core_done && !error) {
		p->last_core_activation_ns = now;
		p->last_core_activation_epoch = profiles->association_epoch;
	}
	if (!error && !strcmp(reason, "commit")) {
		/* authenticated validation passed in this association */
		p->current_validated = true;
		p->support = MT7927_MLO_SUPPORT_SUPPORTED;
		p->consecutive_failures = 0;
		p->backoff_until_ns = 0;
	}
	if (error) {
		struct mt7927_mlo_pair_failure *f = &p->failure[p->failures++ % 4];
		unsigned int shift;

		p->consecutive_failures++;
		shift = p->consecutive_failures - 1;
		if (shift > MT7927_MLO_BACKOFF_MAX_SHIFT)
			shift = MT7927_MLO_BACKOFF_MAX_SHIFT;
		p->backoff_until_ns = now +
			((MT7927_MLO_BACKOFF_BASE_MS * 1000000ULL) << shift);

		p->last_error = error;
		strscpy(p->last_reason, reason, sizeof(p->last_reason));
		f->ns = now;
		f->epoch = profiles->association_epoch;
		f->observation_generation = profiles->observation_generation;
		f->error = error;
		strscpy(f->reason, reason, sizeof(f->reason));
		/* A refused target is not evidence that another pair lost security.
		 * Reset/key/channel loss has a separate global invalidation path.
		 */
		p->current_validated = false;
		profiles->observation_generation++;
	}
}

static struct mt7925_mlo_vif *mt7927_mlo_private(struct ieee80211_vif *vif)
{
	return (void *)vif->drv_priv;
}

static bool mt7927_mlo_vif_supported(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;

	return mvif->phy && is_mt7927(&mvif->phy->dev->mt76) &&
	       vif->type == NL80211_IFTYPE_STATION;
}

void mt7927_mlo_pairs_init(struct ieee80211_vif *vif)
{
	struct mt7925_mlo_vif *priv = mt7927_mlo_private(vif);

	spin_lock_init(&priv->pair_lock);
	mt7927_mlo_profiles_init(&priv->profiles, mt7927_mlo_vif_supported(vif));
}

int mt7927_mlo_profiles_admit(const struct mt7927_mlo_profiles *p, u16 target,
			      u16 valid, u16 usable, bool associated,
			      bool negotiate)
{
	int idx;

	if (!p->device_supported)
		return -ENODEV;
	if (!associated)
		return -ENOTCONN;
	if (!target || (target & ~valid) || hweight16(target) > 2)
		return -EINVAL;
	if (!negotiate && (target & ~usable))
		return -EAGAIN;
	idx = mt7927_mlo_pair_index(target);
	if (idx >= 0 && !p->pair[idx].enabled)
		return -EACCES;
	/* Administrative admission is not firmware/security/data readiness. */
	return 0;
}

static int mt7927_mlo_pairs_admit_phase(struct ieee80211_vif *vif, u16 target,
				      bool negotiate, bool association_setup)
{
	struct mt7925_mlo_vif *priv;
	unsigned long flags;
	int ret;

	if (!mt7927_mlo_vif_supported(vif) || !ieee80211_vif_is_mld(vif))
		return 0;
	priv = mt7927_mlo_private(vif);
	spin_lock_irqsave(&priv->pair_lock, flags);
	ret = mt7927_mlo_profiles_admit(&priv->profiles, target, vif->valid_links,
			ieee80211_vif_usable_links(vif),
			association_setup || vif->cfg.assoc, negotiate);
	spin_unlock_irqrestore(&priv->pair_lock, flags);
	return ret;
}

int mt7927_mlo_pairs_admit(struct ieee80211_vif *vif, u16 target, bool negotiate)
{
	return mt7927_mlo_pairs_admit_phase(vif, target, negotiate, false);
}

/* Only the owned mac80211 STA-ASSOC setup path may use this contract.
 * cfg.assoc is published later by mac80211; this checks administrative
 * membership/usable/profile prerequisites, never authorization/readiness.
 */
int mt7927_mlo_pairs_prepare_association(struct ieee80211_vif *vif, u16 target)
{
	return mt7927_mlo_pairs_admit_phase(vif, target, false, true);
}

void mt7927_mlo_pairs_invalidate(struct ieee80211_vif *vif, bool security)
{
	struct mt7925_mlo_vif *priv;
	unsigned long flags;

	if (!mt7927_mlo_vif_supported(vif))
		return;
	priv = mt7927_mlo_private(vif);
	spin_lock_irqsave(&priv->pair_lock, flags);
	mt7927_mlo_profiles_invalidate(&priv->profiles);
	priv->profiles.association_epoch = READ_ONCE(priv->base.mlo_completion.assoc_epoch);
	if (security) {
		priv->profiles.authorized = false;
		priv->auto_intent = false;
	}
	WRITE_ONCE(priv->base.mlo_auto_steering, false);
	spin_unlock_irqrestore(&priv->pair_lock, flags);
}

/* Autonomous steering: when the AP station is authorized, steering starts by
 * itself if the steering gate holds. debugfs mlo_auto_steering can still stop
 * or start it for a controlled test; it is never required for normal use.
 */
static bool mt7927_mlo_auto_steer = true;
module_param_named(mlo_auto_steer, mt7927_mlo_auto_steer, bool, 0644);
MODULE_PARM_DESC(mlo_auto_steer,
		 "start MT7927 MLO steering automatically once the AP station is authorized; transitions not yet verified in the association stay blocked (default Y)");

void mt7927_mlo_pairs_authorized(struct ieee80211_vif *vif)
{
	struct mt7925_mlo_vif *priv;
	unsigned long flags;

	if (!mt7927_mlo_vif_supported(vif))
		return;
	priv = mt7927_mlo_private(vif);
	spin_lock_irqsave(&priv->pair_lock, flags);
	priv->profiles.authorized = true;
	priv->auto_intent = READ_ONCE(mt7927_mlo_auto_steer);
	if (priv->auto_intent &&
	    mt7927_mlo_profiles_can_steer(&priv->profiles))
		WRITE_ONCE(priv->base.mlo_auto_steering, true);
	spin_unlock_irqrestore(&priv->pair_lock, flags);
	/* Keep the first genuine 0x7 band map; after a reset, report what is
	 * still severe so the remaining options are explicit.
	 */
	mt7925_mlo_severity_lastgood_note(vif);
	mt7925_mlo_severity_post_reset(vif);
}

/* A foreign link/channel event stops the running flag; steering that is still
 * wanted for this authorized association resumes once the gate holds again.
 */
void mt7927_mlo_pairs_autostart(struct ieee80211_vif *vif)
{
	struct mt7925_mlo_vif *priv;
	unsigned long flags;

	if (!mt7927_mlo_vif_supported(vif))
		return;
	priv = mt7927_mlo_private(vif);
	spin_lock_irqsave(&priv->pair_lock, flags);
	if (priv->auto_intent && !READ_ONCE(priv->base.mlo_auto_steering) &&
	    mt7927_mlo_profiles_can_steer(&priv->profiles))
		WRITE_ONCE(priv->base.mlo_auto_steering, true);
	spin_unlock_irqrestore(&priv->pair_lock, flags);
}

void mt7927_mlo_pairs_note(struct ieee80211_vif *vif, u16 target,
			   int error, const char *reason, bool core_done)
{
	struct mt7925_mlo_vif *priv;
	unsigned long flags;
	int idx = mt7927_mlo_pair_index(target);
	u64 now = ktime_get_ns();

	if (idx < 0 || !mt7927_mlo_vif_supported(vif))
		return;
	priv = mt7927_mlo_private(vif);
	spin_lock_irqsave(&priv->pair_lock, flags);
	priv->profiles.association_epoch = READ_ONCE(priv->base.mlo_completion.assoc_epoch);
	mt7927_mlo_profile_note(&priv->profiles, target, error, reason, now, core_done);
	spin_unlock_irqrestore(&priv->pair_lock, flags);
}

bool mt7927_mlo_pairs_can_steer(struct ieee80211_vif *vif)
{
	struct mt7925_mlo_vif *priv;
	unsigned long flags;
	bool ready;

	if (!mt7927_mlo_vif_supported(vif))
		return false;
	priv = mt7927_mlo_private(vif);
	spin_lock_irqsave(&priv->pair_lock, flags);
	ready = mt7927_mlo_profiles_can_steer(&priv->profiles);
	spin_unlock_irqrestore(&priv->pair_lock, flags);
	return ready;
}

bool mt7927_mlo_pair_enabled(struct ieee80211_vif *vif, u16 mask)
{
	struct mt7925_mlo_vif *priv;
	int idx = mt7927_mlo_pair_index(mask);

	if (idx < 0 || !mt7927_mlo_vif_supported(vif))
		return false;
	priv = mt7927_mlo_private(vif);
	return READ_ONCE(priv->profiles.pair[idx].enabled);
}

bool mt7927_mlo_pair_backoff_active(struct ieee80211_vif *vif, u16 mask)
{
	struct mt7925_mlo_vif *priv;
	unsigned long flags;
	int idx = mt7927_mlo_pair_index(mask);
	bool active;

	if (idx < 0 || !mt7927_mlo_vif_supported(vif))
		return false;
	priv = mt7927_mlo_private(vif);
	spin_lock_irqsave(&priv->pair_lock, flags);
	active = priv->profiles.pair[idx].backoff_until_ns &&
		 ktime_get_ns() < priv->profiles.pair[idx].backoff_until_ns;
	spin_unlock_irqrestore(&priv->pair_lock, flags);
	return active;
}

unsigned long mt7927_mlo_steering_interval(struct ieee80211_vif *vif)
{
	struct mt7925_mlo_vif *priv = mt7927_mlo_private(vif);

	return msecs_to_jiffies(READ_ONCE(priv->profiles.interval_ms));
}

struct mt7927_mlo_control_ctx {
	struct mt792x_dev *dev;
	enum mt7927_mlo_control control;
	u16 pair_mask;
};

struct mt7927_mlo_vif_lookup {
	struct ieee80211_vif *vif;
	unsigned int count;
};

static void mt7927_mlo_find_vif(void *data, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt7927_mlo_vif_lookup *lookup = data;

	if (!mt7927_mlo_vif_supported(vif))
		return;
	lookup->vif = vif;
	lookup->count++;
}

/* wiphy serializes VIF lifetime. Never silently choose the first interface
 * when multiple managed VIFs would make a device-wide control ambiguous.
 */
static struct ieee80211_vif *mt7927_mlo_control_vif(struct mt792x_dev *dev)
{
	struct mt7927_mlo_vif_lookup lookup = {};

	lockdep_assert_wiphy(mt76_hw(dev)->wiphy);
	ieee80211_iterate_active_interfaces(mt76_hw(dev),
				IEEE80211_IFACE_ITER_RESUME_ALL,
				mt7927_mlo_find_vif, &lookup);
	if (lookup.count != 1)
		return ERR_PTR(lookup.count ? -ENOTUNIQ : -ENOLINK);
	return lookup.vif;
}

static int mt7927_mlo_control_get(void *data, u64 *value)
{
	struct mt7927_mlo_control_ctx *ctx = data;
	struct mt7927_mlo_profiles *p;
	struct ieee80211_vif *vif;
	struct mt7925_mlo_vif *priv;
	unsigned long flags;
	int ret = 0;

	if (!is_mt7927(&ctx->dev->mt76))
		return -ENODEV;
	wiphy_lock(mt76_hw(ctx->dev)->wiphy);
	vif = mt7927_mlo_control_vif(ctx->dev);
	if (IS_ERR(vif)) {
		ret = PTR_ERR(vif);
		goto out;
	}
	priv = mt7927_mlo_private(vif);
	p = &priv->profiles;
	spin_lock_irqsave(&priv->pair_lock, flags);
	switch (ctx->control) {
	case MT7927_MLO_CTL_PAIR03:
	case MT7927_MLO_CTL_PAIR05:
	case MT7927_MLO_CTL_PAIR06: *value = p->pair[ctx->control].enabled; break;
	case MT7927_MLO_CTL_ENABLE: *value = priv->base.mlo_auto_steering; break;
	case MT7927_MLO_CTL_POLICY: *value = p->policy; break;
	case MT7927_MLO_CTL_INTERVAL: *value = p->interval_ms; break;
	case MT7927_MLO_CTL_FALLBACK: *value = p->fallback_pair; break;
	}
	spin_unlock_irqrestore(&priv->pair_lock, flags);
out:
	wiphy_unlock(mt76_hw(ctx->dev)->wiphy);
	return ret;
}

static int mt7927_mlo_control_set(void *data, u64 value)
{
	struct mt7927_mlo_control_ctx *ctx = data;
	struct ieee80211_vif *vif;
	struct mt7925_mlo_vif *priv;
	unsigned long flags;
	int ret;

	if (!is_mt7927(&ctx->dev->mt76))
		return -ENODEV;
	wiphy_lock(mt76_hw(ctx->dev)->wiphy);
	vif = mt7927_mlo_control_vif(ctx->dev);
	if (IS_ERR(vif)) {
		ret = PTR_ERR(vif);
		goto out;
	}
	priv = mt7927_mlo_private(vif);
	spin_lock_irqsave(&priv->pair_lock, flags);
	ret = mt7927_mlo_profiles_configure(&priv->profiles, ctx->control, value);
	if (!ret && ctx->control == MT7927_MLO_CTL_ENABLE) {
		priv->auto_intent = !!value;
		WRITE_ONCE(priv->base.mlo_auto_steering, !!value);
	}
	if (!ret && !mt7927_mlo_profiles_can_steer(&priv->profiles))
		WRITE_ONCE(priv->base.mlo_auto_steering, false);
	spin_unlock_irqrestore(&priv->pair_lock, flags);
	if (!ret) {
		/* Match the old debugfs path's mutex discipline for monitor I/O.
		 * Do not join ordinary work here: its completion uses wiphy.
		 */
		mt792x_mutex_acquire(ctx->dev);
		mt7925_mlo_update_rssi_monitor(ctx->dev, vif);
		mt792x_mutex_release(ctx->dev);
		mt7927_mlo_steering_kick(vif);
	}
out:
	wiphy_unlock(mt76_hw(ctx->dev)->wiphy);
	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(mt7927_mlo_control_fops, mt7927_mlo_control_get,
			mt7927_mlo_control_set, "%llu\n");

static void mt7927_mlo_pair_link_show(struct seq_file *s,
				    struct ieee80211_vif *vif, unsigned int id,
				    bool authorized, bool *ready)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_phy *phy = mvif->phy;
	struct mt7927_mlo_link_state *ls = &mvif->mt7927_mlo.link[id];
	struct ieee80211_bss_conf *conf = mt792x_vif_to_bss_conf(vif, id);
	struct mt792x_bss_conf *mconf = mt792x_vif_to_link(mvif, id);
	struct ieee80211_sta *sta = ieee80211_find_sta(vif, vif->cfg.ap_addr);
	struct mt792x_sta *msta = sta ? (void *)sta->drv_priv : NULL;
	struct mt792x_link_sta *link = mt792x_sta_link_rcu(msta, id);
	struct ieee80211_channel *chan;
	bool live_peer = link && link->wcid.link_valid;
	bool keyed = link && link->wcid.hw_key_idx != 255;
	unsigned long rx = link ? READ_ONCE(link->mlo_valid_rx_jiffies) : 0;
	bool fresh = rx && mt7925_mlo_pt_secure_rx_fresh(1,
					jiffies_to_msecs(jiffies - rx));
	bool held = false;
	bool context_owned;
	unsigned int g;

	/* The legacy lookup can fall back to the deflink. Never report its
	 * BSS/channel/radio as ownership of an absent requested member.
	 */
	if (!mt7927_mlo_context_link_matches(id, conf ? conf->link_id : -1))
		conf = NULL;
	if (!mt7927_mlo_context_link_matches(id, mconf ? mconf->link_id : -1))
		mconf = NULL;
	chan = conf ? conf->chanreq.oper.chan : NULL;
	context_owned = chan && mconf && mconf->mt76.ctx &&
		mconf->mt76.ctx->def.chan &&
		mconf->mt76.ctx->def.chan->center_freq == chan->center_freq;

	seq_printf(s, " link=%u frequency=%d width=%d host_bss=%d host_band=%d host_omac=%d host_wmm=%d assigned_context_matches=%u\n",
		   id, chan ? chan->center_freq : 0,
		   chan ? conf->chanreq.oper.width : -1,
		   mconf ? mconf->mt76.idx : -1, mconf ? mconf->mt76.band_idx : -1,
		   mconf ? mconf->mt76.omac_idx : -1, mconf ? mconf->mt76.wmm_idx : -1,
		   context_owned);
	for (g = 0; g < ARRAY_SIZE(phy->mlo_roc.grant); g++) {
		struct mt7925_mlo_grant_inc *grant = &phy->mlo_roc.grant[g];

		if (grant->link_id != id || !grant->token || !mconf ||
		    grant->bss_idx != mconf->mt76.idx || !chan ||
		    grant->channel != chan->hw_value)
			continue;
		seq_printf(s, "  roc_token=%u bss=%u dbdcband=%u channel=%u life=%u continuity_owner=%u secure_rx=%u\n",
			   grant->token, grant->bss_idx, grant->dbdcband,
			   grant->channel, grant->life, grant->continuity_owner,
			   grant->secure_rx);
		if ((phy->mlo_roc.held_links & BIT(id)) &&
		    id < ARRAY_SIZE(phy->mlo_roc.link_token) &&
		    grant->token == (phy->mlo_roc.link_token[id] ?:
				     phy->mlo_roc.held_token) &&
		    (grant->life == MT7925_MLO_GRANT_HELD ||
		    grant->life == MT7925_MLO_GRANT_HELD_CONTINUITY ||
		    grant->life == MT7925_MLO_GRANT_VALIDATED))
			held = true;
	}
	seq_printf(s, "  wcid=%d peer_valid=%u hw_key_idx=%d host_ptk_synced=%u host_group_synced=%u authorize_event=%u host_link_ready=%u roc_match=%u roc_admitted=%u admitted_generation=%u\n",
		   link ? link->wcid.idx : -1, live_peer,
		   link ? link->wcid.hw_key_idx : -1, ls->ptk_synced,
		   ls->group_synced, authorized, ls->link_ready, held,
		   ls->roc_admitted, ls->admitted_generation);
	if (link)
		seq_printf(s, "  tx_packets=%u tx_bytes=%llu rx_packets=%u rx_bytes=%llu cipher_valid_rx_pre_host_pn=%u rx_age_ms=%lu fresh_rx=%u ampdu=0x%lx\n",
			   READ_ONCE(link->wcid.stats.tx_packets),
			   (unsigned long long)READ_ONCE(link->wcid.stats.tx_bytes),
			   READ_ONCE(link->wcid.stats.rx_packets),
			   (unsigned long long)READ_ONCE(link->wcid.stats.rx_bytes),
			   READ_ONCE(link->mlo_valid_rx_packets),
			   rx ? jiffies_to_msecs(jiffies - rx) : ~0UL,
			   fresh, READ_ONCE(link->wcid.ampdu_state));
	*ready &= context_owned && live_peer && keyed && authorized && held &&
		  ls->link_ready && ls->ptk_synced && fresh;
}

static int mt7927_mlo_pairs_show(struct seq_file *s, void *unused)
{
	struct mt7927_mlo_control_ctx *ctx = s->private;
	struct mt792x_dev *dev = ctx->dev;
	struct ieee80211_vif *vif;
	struct mt7925_mlo_vif *priv;
	struct mt7927_mlo_profiles *p;
	unsigned int i, id, f;
	unsigned long flags;
	u64 epoch;
	int ret = 0;

	if (!is_mt7927(&dev->mt76))
		return -ENODEV;
	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	wiphy_lock(mt76_hw(dev)->wiphy);
	vif = mt7927_mlo_control_vif(dev);
	if (IS_ERR(vif)) {
		ret = PTR_ERR(vif);
		goto out;
	}
	priv = mt7927_mlo_private(vif);
	spin_lock_irqsave(&priv->pair_lock, flags);
	*p = priv->profiles;
	spin_unlock_irqrestore(&priv->pair_lock, flags);
	spin_lock_irqsave(&priv->base.mlo_completion.lock, flags);
	epoch = priv->base.mlo_completion.assoc_epoch;
	spin_unlock_irqrestore(&priv->base.mlo_completion.lock, flags);
	/* Host-only observation: no MCU request or register/key/PN read. */
	mutex_lock(&dev->mt76.mutex);
	seq_printf(s, "device=%s epoch=%llu observation_generation=%llu associated=%u valid_links=0x%x active_links=0x%x steering=%u policy=%u interval_ms=%u fallback=0x%x\n",
		   dev_name(dev->mt76.dev), epoch, p->observation_generation,
		   vif->cfg.assoc, vif->valid_links, vif->active_links,
		   READ_ONCE(priv->base.mlo_auto_steering), p->policy,
		   p->interval_ms, p->fallback_pair);
	seq_puts(s, "proof_limits=host_snapshot_not_firmware_readback key_residency=unknown ap_pm=unobserved post_pn_core_rx=external tcp_validation=external tx_wcid_counters_not_air_link_proof=1\n");
	seq_puts(s, "evidence_classes=historical_observation,prior_implementation_epoch,new_subsystem_acceptance new_subsystem_hardware_acceptance=unestablished\n");
	seq_printf(s, "roc_held_token=%u held_links=0x%x pending_token=%u pending_links=0x%x timedout_token=%u single_token=%u single_state=%u single_status=%u single_bss=%u single_event_bss=%u single_channel=%u single_dbdc=%u roc_snapshot_atomic=0\n",
		   dev->phy.mlo_roc.held_token, dev->phy.mlo_roc.held_links,
		   dev->phy.mlo_roc.pending_token, dev->phy.mlo_roc.pending_links,
		   dev->phy.mlo_roc.timedout_token, dev->phy.mlo_roc.single_token,
		   dev->phy.mlo_roc.single_state, dev->phy.mlo_roc.single_status,
		   dev->phy.mlo_roc.single_bss, dev->phy.mlo_roc.single_ev_bss,
		   dev->phy.mlo_roc.single_ch, dev->phy.mlo_roc.single_dbdc);
	rcu_read_lock();
	for (i = 0; i < ARRAY_SIZE(p->pair); i++) {
		struct mt7927_mlo_pair_profile *profile = &p->pair[i];
		unsigned long bits = profile->mask;
		bool eligible = profile->enabled && vif->cfg.assoc &&
			ieee80211_vif_is_mld(vif) &&
			!(profile->mask & ~ieee80211_vif_usable_links(vif));
		bool ready = eligible && vif->active_links == profile->mask &&
			READ_ONCE(priv->base.mlo_completion.enabled) &&
			!test_bit(MT76_RESET, &dev->mphy.state) &&
			!test_bit(MT76_REMOVED, &dev->mphy.state) &&
			!dev->hw_full_reset && !dev->pm.suspended &&
			!atomic_read(&dev->mt76.bus_hung);

		if (ctx->pair_mask && ctx->pair_mask != profile->mask)
			continue;

		seq_printf(s, "pair=0x%x enabled=%u support=%s eligible=%u active=%u current_validated=%u historical_project_memory=%u historical_scope=external_not_current last_attempt_ns=%llu last_attempt_epoch=%llu last_core_activation_ns=%llu last_core_activation_epoch=%llu last_error=%d last_reason=%s failure_count=%u consecutive_failures=%u backoff_remaining_ms=%llu\n",
			   profile->mask, profile->enabled,
			   profile->support == MT7927_MLO_SUPPORT_SUPPORTED ? "supported" :
			   profile->support == MT7927_MLO_SUPPORT_UNSUPPORTED ? "unsupported" : "unknown",
			   eligible, vif->active_links == profile->mask,
			   profile->current_validated, profile->historical_memory,
			   profile->last_attempt_ns, profile->last_attempt_epoch,
			   profile->last_core_activation_ns, profile->last_core_activation_epoch,
			   profile->last_error, profile->last_reason, profile->failures,
			   profile->consecutive_failures,
			   profile->backoff_until_ns > ktime_get_ns() ?
				div_u64(profile->backoff_until_ns - ktime_get_ns(), 1000000) : 0);
		for_each_set_bit(id, &bits, IEEE80211_MLD_MAX_NUM_LINKS)
			mt7927_mlo_pair_link_show(s, vif, id, p->authorized, &ready);
		seq_printf(s, " host_prerequisites=%u ready=%u readiness_requires=current_epoch_stationary_validation\n",
			   ready, ready && profile->current_validated);
		seq_printf(s, " historical_project_status=%s support_not_inferred_from_history=1\n",
			   profile->historical_memory ? "stationary_pass_external_scope" : "unvalidated");
		seq_printf(s, " prior_implementation_memory=%u prior_implementation_scope=external_epoch_bounded_not_new_subsystem_acceptance\n",
			   profile->prior_implementation_memory);
		for (f = 0; f < ARRAY_SIZE(profile->failure); f++)
			if (profile->failure[f].ns)
				seq_printf(s, " failure_ns=%llu epoch=%llu observation_generation=%llu error=%d reason=%s\n",
					   profile->failure[f].ns,
					   profile->failure[f].epoch,
					   profile->failure[f].observation_generation,
					   profile->failure[f].error,
					   profile->failure[f].reason);
	}
	if (vif)
		mt7925_mlo_severity_show(s, vif);
	rcu_read_unlock();
	mutex_unlock(&dev->mt76.mutex);
out:
	wiphy_unlock(mt76_hw(dev)->wiphy);
	kfree(p);
	return ret;
}

DEFINE_SHOW_ATTRIBUTE(mt7927_mlo_pairs);

/* Read-only: the firmware's own MLO capability record, undecorated. This is
 * where the local EML capability comes from; nothing here is inferred.
 */
static int mt7927_mlo_fw_caps_show(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	struct mt792x_phy *phy = &dev->phy;

	if (!phy->mlo_fw_cap.valid) {
		seq_puts(s, "no NIC_CAP tag 0x22 (MLO capability) reported by firmware\n");
		return 0;
	}
	seq_printf(s, "raw=%*phN\n", (int)sizeof(phy->mlo_fw_cap.raw), phy->mlo_fw_cap.raw);
	seq_printf(s, "nonap_eml_support=%u nonap_eml_cap=0x%04x\n",
		   phy->mlo_fw_cap.nonap_eml_support, phy->mlo_fw_cap.nonap_eml_cap);
	seq_printf(s, "ap_eml_support=%u ap_eml_cap=0x%04x\n",
		   phy->mlo_fw_cap.ap_eml_support, phy->mlo_fw_cap.ap_eml_cap);
	seq_printf(s, "max_simul_links=%u link3_band_limit=0x%02x hybrid_mlo=%u\n",
		   phy->mlo_fw_cap.max_simul_links, phy->mlo_fw_cap.link3_band_limit,
		   phy->mlo_fw_cap.hybrid_mlo);
	seq_printf(s, "chip_cap=0x%llx chip_cap_mlo_eml_en=%d\n",
		   (unsigned long long)phy->chip_cap,
		   !!(phy->chip_cap & MT792x_CHIP_CAP_MLO_EML_EN));
	return 0;
}

/* TEST-ONLY: negotiate a TID-to-link mapping that puts every TID on the
 * written link mask, without changing the active links. mac80211 takes the
 * wiphy lock inside the request, so the vif is resolved first and the lock is
 * dropped before asking. The AP's answer is logged by MLO_TTLM_REQ and by
 * mac80211; nothing here assumes it was accepted.
 */
static int mt7927_mlo_ttlm_request_set(void *data, u64 value)
{
	struct mt792x_dev *dev = data;
	struct ieee80211_vif *vif;
	int err;

	wiphy_lock(mt76_hw(dev)->wiphy);
	vif = mt7927_mlo_control_vif(dev);
	wiphy_unlock(mt76_hw(dev)->wiphy);
	if (IS_ERR(vif))
		return PTR_ERR(vif);
	if (vif->type != NL80211_IFTYPE_STATION || !ieee80211_vif_is_mld(vif) ||
	    !vif->cfg.assoc)
		return -ENOLINK;

	err = mt7925_mlo_ttlm_request(vif, value & 0xffff);
	dev_info(dev->mt76.dev,
		 "MLO_TTLM_REQ: links=0x%llx valid=0x%x active=0x%x dormant=0x%x err=%d\n",
		 value & 0xffff, vif->valid_links, vif->active_links,
		 vif->dormant_links, err);
	return err;
}
DEFINE_DEBUGFS_ATTRIBUTE(mt7927_mlo_ttlm_request_fops, NULL,
			 mt7927_mlo_ttlm_request_set, "%llu\n");

int mt7927_mlo_pairs_debugfs_init(struct mt792x_dev *dev, struct dentry *dir)
{
	static const char * const names[] = {
		"mlo_pair_03_enable", "mlo_pair_05_enable", "mlo_pair_06_enable",
		"mlo_steering_enable", "mlo_steering_policy",
		"mlo_steering_interval_ms", "mlo_steering_fallback_pair",
	};
	static const char * const states[] = {
		"mlo_pair_state", "mlo_pair_03_state", "mlo_pair_05_state", "mlo_pair_06_state",
	};
	struct mt7927_mlo_control_ctx *ctx;
	unsigned int i;

	if (!is_mt7927(&dev->mt76))
		return 0;
	ctx = devm_kcalloc(dev->mt76.dev, ARRAY_SIZE(names) + ARRAY_SIZE(states),
			   sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ctx[i].dev = dev;
		ctx[i].control = i;
		debugfs_create_file(names[i], 0600, dir, &ctx[i], &mt7927_mlo_control_fops);
	}
	debugfs_create_devm_seqfile(dev->mt76.dev, "mlo_fw_caps", dir, mt7927_mlo_fw_caps_show);
	debugfs_create_file("mlo_ttlm_request", 0200, dir, dev, &mt7927_mlo_ttlm_request_fops);
	/* Legacy spelling is an alias to the same gate/controller. */
	debugfs_create_file("mlo_auto_steering", 0600, dir,
			    &ctx[MT7927_MLO_CTL_ENABLE], &mt7927_mlo_control_fops);
	for (i = 0; i < ARRAY_SIZE(states); i++) {
		struct mt7927_mlo_control_ctx *view = &ctx[ARRAY_SIZE(names) + i];

		view->dev = dev;
		view->pair_mask = i == 0 ? 0 : i == 1 ? 3 : i == 2 ? 5 : 6;
		debugfs_create_file(states[i], 0400, dir, view, &mt7927_mlo_pairs_fops);
	}
	return 0;
}
