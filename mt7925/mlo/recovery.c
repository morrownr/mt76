// SPDX-License-Identifier: BSD-3-Clause-Clear
/* MLO-owned recovery after a failed replacement admission.
 *
 * mac80211 ignores the sta-links callback error, so the completion owner
 * restores the survivor mask (completion.c). That repairs the published state
 * only: on hardware the survivor then keeps transmitting while its security-valid RX
 * never advances, and a full scan returns no BSS, i.e. firmware radio state is stale
 * and nothing in the generic stack notices. This wrapper watches that survivor
 * and, with TX-without-RX evidence (summed over the MLD peer) over a full window, asks for the one remedy
 * that was observed to clear it: the firmware reset (WFSYS reset + firmware reload).
 * The reset reassociates, so it is evidence-gated, once per association and
 * rate limited. The decision is the pure policy in mlo_recovery_policy.h.
 */
#include "../mt7925.h"
#include "../mlo_recovery_policy.h"
#include "pairs.h"

#define MT7925_MLO_REC_POLL_MS	1000
#define MT7925_MLO_REC_LEAVE_MS	500

static struct mt7925_mlo_recovery_state *
mt7925_mlo_rec(struct ieee80211_vif *vif)
{
	return &((struct mt7925_mlo_vif *)vif->drv_priv)->recovery;
}

static u64 mt7925_mlo_rec_now_ms(void)
{
	return div_u64(ktime_get_ns(), NSEC_PER_MSEC);
}

/* Security-valid RX and TX packet counters of the whole MLD peer: every link's
 * WCID, retired ones included. Hardware showed the host keeps dispatching on the
 * retired link's WCID after the survivor mask was restored while the survivor's own
 * TX counter stood still, so a per-survivor TX count never shows the evidence.
 * Progress of the RX sum on any link means the radio is hearing the AP.
 */
static bool mt7925_mlo_rec_counters(struct ieee80211_vif *vif, u32 *rx, u32 *tx)
{
	struct ieee80211_sta *sta;
	struct mt792x_sta *msta;
	unsigned int link;
	bool ok = false;

	*rx = 0;
	*tx = 0;
	rcu_read_lock();
	sta = ieee80211_find_sta(vif, vif->cfg.ap_addr);
	msta = sta ? (struct mt792x_sta *)sta->drv_priv : NULL;
	for (link = 0; msta && link < IEEE80211_MLD_MAX_NUM_LINKS; link++) {
		struct mt792x_link_sta *mlink = mt792x_sta_link_rcu(msta, link);

		if (!mlink)
			continue;
		*rx += READ_ONCE(mlink->mlo_valid_rx_packets);
		*tx += READ_ONCE(mlink->wcid.stats.tx_packets);
		ok = true;
	}
	rcu_read_unlock();
	return ok;
}

static u64 mt7925_mlo_rec_epoch(struct mt792x_vif *mvif, bool *live)
{
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	unsigned long flags;
	u64 epoch;

	spin_lock_irqsave(&c->lock, flags);
	*live = c->enabled;
	epoch = c->assoc_epoch;
	spin_unlock_irqrestore(&c->lock, flags);
	return epoch;
}

/* Leave the AP cleanly before the reset. The reset drops the association
 * locally and never tells the AP, which then keeps the old station state until
 * its own timeout; the reassociation that follows can be answered as a stale
 * station (a class 2 deauth) when it comes back with the same link addresses.
 * The request makes mac80211 send a deauth (reason 3) and flush it out before
 * the association state drops, so waiting for that state is waiting for the
 * frame. Bounded: the reset follows regardless of the outcome. An association
 * that is already gone has nothing to leave.
 */
static void mt7925_mlo_recovery_clean_leave(struct ieee80211_vif *vif)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(MT7925_MLO_REC_LEAVE_MS);

	if (!READ_ONCE(vif->cfg.assoc))
		return;
	ieee80211_disconnect(vif, false);
	while (READ_ONCE(vif->cfg.assoc) && time_before(jiffies, deadline))
		msleep(10);
	printk(KERN_INFO "MLO_RECOVERY: clean_leave assoc=%d\n",
	       READ_ONCE(vif->cfg.assoc));
}

static void mt7925_mlo_recovery_work(struct work_struct *work)
{
	struct mt7925_mlo_recovery_state *rs =
		container_of(to_delayed_work(work), struct mt7925_mlo_recovery_state, work);
	struct mt7925_mlo_vif *priv = container_of(rs, struct mt7925_mlo_vif, recovery);
	struct mt792x_vif *mvif = &priv->base;
	struct ieee80211_vif *vif = container_of((void *)mvif, struct ieee80211_vif, drv_priv);
	struct mt792x_dev *dev = mvif->phy->dev;
	enum mt7925_mlo_rec_action act;
	u64 now = mt7925_mlo_rec_now_ms(), epoch;
	u32 rx = 0, tx = 0;
	bool live;

	if (rs->reassoc_req) {
		rs->reassoc_req = false;
		printk(KERN_INFO "MLO_RECOVERY: action=reassoc previous=0x%x failed_target=0x%x\n",
		       rs->prev, rs->failed);
		/* First caller: the pair being left is marked severe, then the station
		 * leaves cleanly; the second caller decides the pair at the new
		 * association. The radio is healthy, so no firmware reset.
		 */
		mt7925_mlo_severity_mark_previous(vif, rs->prev);
		mt7925_mlo_recovery_clean_leave(vif);
		return;
	}

	epoch = mt7925_mlo_rec_epoch(mvif, &live);
	if (!live || !vif->cfg.assoc) {
		/* Not an association we armed in: bind to an impossible epoch. */
		epoch = ~rs->policy.epoch;
	}
	if (!mt7925_mlo_rec_counters(vif, &rx, &tx)) {
		rx = rs->policy.rx_last;	/* peer gone: repeat the last sample */
		tx = rs->policy.tx_last;
		epoch = ~rs->policy.epoch;
	}

	act = mt7925_mlo_recovery_step(&rs->policy, rx, tx, now, epoch,
				       rs->reset_seen ? now - rs->last_reset_ms : ~0ULL);
	switch (act) {
	case MT7925_MLO_REC_WAIT:
		ieee80211_queue_delayed_work(mt76_hw(dev), &rs->work,
					     msecs_to_jiffies(MT7925_MLO_REC_POLL_MS));
		return;
	case MT7925_MLO_REC_RESET:
		printk(KERN_INFO "MLO_RECOVERY: action=reset link=%u previous=0x%x failed_target=0x%x rx=%u tx_delta=%u window_ms=%u\n",
		       rs->link, rs->prev, rs->failed, rx, tx - rs->policy.tx_mark,
		       MT7925_MLO_REC_WINDOW_MS);
		mt7927_mlo_pairs_note(vif, BIT(rs->link), -ENODATA,
				      "recovery_firmware_reset", false);
		/* First caller: the connection being left is the previous one, so that
		 * pair is marked severe before the reset; the second caller decides
		 * what to run afterwards.
		 */
		mt7925_mlo_severity_mark_previous(vif, rs->prev);
		mt7925_mlo_recovery_clean_leave(vif);
		rs->last_reset_ms = now;
		rs->reset_seen = true;
		mt792x_reset(&dev->mt76);
		return;
	case MT7925_MLO_REC_BACKOFF:
		printk(KERN_INFO "MLO_RECOVERY: action=backoff link=%u reset_not_allowed\n", rs->link);
		return;
	case MT7925_MLO_REC_HEALTHY:
		printk(KERN_INFO "MLO_RECOVERY: action=healthy link=%u rx_progress=1\n", rs->link);
		return;
	case MT7925_MLO_REC_GIVE_UP:
		printk(KERN_INFO "MLO_RECOVERY: action=give_up link=%u no_tx_evidence\n", rs->link);
		return;
	case MT7925_MLO_REC_CANCEL:
	case MT7925_MLO_REC_IDLE:
	default:
		return;
	}
}

void mt7925_mlo_recovery_init(struct ieee80211_vif *vif)
{
	struct mt7925_mlo_recovery_state *rs = mt7925_mlo_rec(vif);

	if (rs->initialized)
		return;
	INIT_DELAYED_WORK(&rs->work, mt7925_mlo_recovery_work);
	rs->initialized = true;
}

/* Arm after the survivor mask was restored. One survivor link is watched (the
 * lowest set bit); epoch is the association epoch the failure belongs to.
 */
void mt7925_mlo_recovery_arm_survivor(struct ieee80211_vif *vif, u16 survivors,
				      u16 failed, u16 prev, u64 epoch)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt7925_mlo_recovery_state *rs = mt7925_mlo_rec(vif);
	u32 rx, tx;

	if (!rs->initialized || !survivors || !is_mt7927(&mvif->phy->dev->mt76))
		return;
	rs->link = __ffs(survivors);
	rs->failed = failed;
	rs->prev = prev;
	if (!mt7925_mlo_rec_counters(vif, &rx, &tx))
		return;
	mt7925_mlo_recovery_arm(&rs->policy, rx, tx, mt7925_mlo_rec_now_ms(), epoch);
	printk(KERN_INFO "MLO_RECOVERY: armed link=%u rx=%u tx=%u\n", rs->link, rx, tx);
	ieee80211_queue_delayed_work(mt76_hw(mvif->phy->dev), &rs->work,
				     msecs_to_jiffies(MT7925_MLO_REC_POLL_MS));
}

/* A transition failed at the grant stage while the radio is healthy: the
 * firmware answers no delta ACQUIRE next to a held survivor (either band) and
 * grants only through the JOIN built at association time, so the way back to a
 * full pair is one reassociation. Called with the wiphy lock held, so it only
 * queues the worker. Rate limited by the pure policy.
 */
void mt7925_mlo_recovery_request_reassoc(struct ieee80211_vif *vif, u16 prev, u16 failed)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt7925_mlo_recovery_state *rs = mt7925_mlo_rec(vif);
	u64 now = mt7925_mlo_rec_now_ms();
	bool live;
	u64 epoch;

	if (!rs->initialized || !is_mt7927(&mvif->phy->dev->mt76) || !vif->cfg.assoc)
		return;
	epoch = mt7925_mlo_rec_epoch(mvif, &live);
	if (!mt7925_mlo_reassoc_allowed(rs->reassoc_seen ? now - rs->last_reassoc_ms : ~0ULL)) {
		printk(KERN_INFO "MLO_RECOVERY: action=reassoc_backoff previous=0x%x failed_target=0x%x\n",
		       prev, failed);
		return;
	}
	rs->prev = prev;
	rs->failed = failed;
	rs->last_reassoc_ms = now;
	rs->reassoc_seen = true;
	rs->reassoc_epoch = live ? epoch : ~0ULL;
	rs->reassoc_req = true;
	cancel_delayed_work(&rs->work);
	ieee80211_queue_delayed_work(mt76_hw(mvif->phy->dev), &rs->work, 0);
}

/* A reassociation was requested in the current association. Bound to the
 * association epoch, so it is false again as soon as the new association starts.
 * Policy V2's validator uses it to skip a rollback that cannot be granted.
 */
bool mt7925_mlo_recovery_reassoc_pending(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt7925_mlo_recovery_state *rs = mt7925_mlo_rec(vif);
	bool live;
	u64 epoch;

	if (!rs->initialized || !rs->reassoc_seen)
		return false;
	epoch = mt7925_mlo_rec_epoch(mvif, &live);
	return live && epoch == rs->reassoc_epoch;
}

/* The association went away while a watch was armed (atomic-safe). Do not drop
 * the watch: run its final evaluation now. Hardware showed the radio stays dead
 * after the disassociation, so TX-without-RX that was already seen still earns
 * the reset; the worker sees the changed epoch and the policy decides.
 */
void mt7925_mlo_recovery_assoc_lost(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt7925_mlo_recovery_state *rs = mt7925_mlo_rec(vif);

	if (!rs->initialized || !rs->policy.armed)
		return;
	cancel_delayed_work(&rs->work);
	ieee80211_queue_delayed_work(mt76_hw(mvif->phy->dev), &rs->work, 0);
}

/* Teardown (wiphy held): the worker never takes the wiphy lock. */
void mt7925_mlo_recovery_stop(struct ieee80211_vif *vif)
{
	struct mt7925_mlo_recovery_state *rs = mt7925_mlo_rec(vif);

	if (rs->initialized)
		cancel_delayed_work_sync(&rs->work);
}
