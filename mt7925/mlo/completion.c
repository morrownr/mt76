// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "../mt7925.h"
#include "pairs.h"

static struct mt7925_mlo_admit_fail *
mt7925_mlo_admit_fail(struct ieee80211_vif *vif)
{
	return &((struct mt7925_mlo_vif *)vif->drv_priv)->admit_fail;
}

/* A replacement link did not reach secure RX. Record what the completion
 * owner must restore once the outer setter (and its incoming key switching)
 * has returned; never touch vif->active_links from inside the callback.
 */
void mt7925_mlo_completion_admission_failed(struct ieee80211_vif *vif,
					  u16 old_links, u16 new_links,
					  u16 prev_links, int error)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	struct mt7925_mlo_admit_fail *af = mt7925_mlo_admit_fail(vif);
	u16 survivor = old_links & new_links;
	unsigned long flags;
	bool queue = false;

	if (!error || !survivor || !(new_links & ~old_links) ||
	    !smp_load_acquire(&c->initialized))
		return;
	spin_lock_irqsave(&c->lock, flags);
	/* A superseded owned setter's late failure is not a new request. */
	if (!c->enabled ||
	    (c->executing && c->result.phase != MT792X_MLO_CORE_RUNNING))
		goto out;
	*af = (struct mt7925_mlo_admit_fail) {
		.pending = true, .failed = new_links, .prev = prev_links,
		.survivor = survivor,
		.error = error, .epoch = c->assoc_epoch,
	};
	queue = !c->executing;
out:
	spin_unlock_irqrestore(&c->lock, flags);
	if (queue)
		wiphy_work_queue(mt76_hw(mvif->phy->dev)->wiphy, &c->work);
}

static bool mt7925_mlo_ticket_equal(const struct mt792x_mlo_ticket *a,
				   const struct mt792x_mlo_ticket *b)
{
	return a->cookie == b->cookie && a->assoc_epoch == b->assoc_epoch &&
	       a->target == b->target && a->expected_active == b->expected_active;
}

static bool mt7925_mlo_completion_device_ready(struct mt792x_vif *mvif)
{
	struct mt792x_dev *dev = mvif->phy->dev;

	/* These are legacy bitfields, not READ_ONCE-compatible objects. Reset
	 * invalidation also closes the spinlock-protected producer/epoch gate.
	 */
	return !dev->hw_full_reset && !dev->pm.suspended &&
	       !test_bit(MT76_RESET, &dev->mphy.state) &&
	       !test_bit(MT76_REMOVED, &dev->mphy.state) &&
	       !atomic_read(&dev->mt76.bus_hung);
}

/* Restore the survivor mask after a failed replacement admission. Called with
 * wiphy held and c->executing set, so the repair's own callbacks are not
 * foreign. Consumes the record once; stale epoch/mask means someone else
 * changed the topology and nothing is touched.
 */
static int mt7925_mlo_admit_repair(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	struct mt7925_mlo_admit_fail af, *rec = mt7925_mlo_admit_fail(vif);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&c->lock, flags);
	af = *rec;
	rec->pending = false;
	if (!af.pending || !c->enabled || af.epoch != c->assoc_epoch) {
		spin_unlock_irqrestore(&c->lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&c->lock, flags);

	if (!vif->cfg.assoc || !mt7925_mlo_completion_device_ready(mvif))
		ret = -ECANCELED;
	else if (vif->active_links != af.failed)
		ret = -ESTALE;
	else if (af.survivor & ~ieee80211_vif_usable_links(vif))
		ret = -EINVAL;
	else
		ret = ieee80211_set_active_links(vif, af.survivor);

	mt7927_mlo_pairs_note(vif, af.survivor, ret ?: af.error,
			      "admission_failed_restore", false);
	/* The mask is repaired, not the radio: watch the survivor. */
	if (!ret)
		mt7925_mlo_recovery_arm_survivor(vif, af.survivor, af.failed, af.prev,
						 af.epoch);
	return ret;
}

static void mt7925_mlo_completion_work(struct wiphy *wiphy,
				     struct wiphy_work *work)
{
	struct mt792x_mlo_completion *c =
		container_of(work, struct mt792x_mlo_completion, work);
	struct mt792x_vif *mvif = container_of(c, struct mt792x_vif, mlo_completion);
	struct ieee80211_vif *vif =
		container_of((void *)mvif, struct ieee80211_vif, drv_priv);
	struct mt792x_mlo_ticket ticket;
	unsigned long flags;
	bool owned;
	int ret = 0;

	lockdep_assert_wiphy(wiphy);
	spin_lock_irqsave(&c->lock, flags);
	owned = c->enabled && c->result.phase == MT792X_MLO_CORE_QUEUED;
	if (!owned &&
	    !(c->enabled && mt7925_mlo_admit_fail(vif)->pending)) {
		spin_unlock_irqrestore(&c->lock, flags);
		return;
	}
	if (owned) {
		ticket = c->result.ticket;
		c->result.phase = MT792X_MLO_CORE_RUNNING;
	}
	c->executing = true;
	spin_unlock_irqrestore(&c->lock, flags);

	/* No mt76 mutex here: the real driver callbacks acquire it themselves. */
	if (owned) {
		if (!vif->cfg.assoc || !mt7925_mlo_completion_device_ready(mvif))
			ret = -ECANCELED;
		else if (vif->active_links != ticket.expected_active)
			ret = -ESTALE;
		else
			ret = ieee80211_set_active_links(vif, ticket.target);
	}

	/* Failed replacement admission: mac80211 ignored the callback error
	 * and published the requested mask. Restore the survivor now, after the
	 * outer setter and its incoming key switching.
	 */
	mt7925_mlo_admit_repair(vif);

	/* This boundary is AFTER the outer setter, including incoming key
	 * switching. Neither a callback nor the provisional mask reaches it.
	 * A reset/superseding producer may have invalidated the ticket meanwhile.
	 */
	spin_lock_irqsave(&c->lock, flags);
	c->executing = false;
	if (owned && c->enabled &&
	    mt7925_mlo_ticket_equal(&c->result.ticket, &ticket) &&
	    c->result.phase == MT792X_MLO_CORE_RUNNING) {
		c->result.core_ret = ret;
		c->result.final_active = vif->active_links;
		c->result.finished_jiffies = jiffies;
		c->result.phase = MT792X_MLO_CORE_DONE;
		mt7927_mlo_pairs_note(vif, ticket.target,
				      ret ?: c->result.firmware_error,
				      "outer_activation_return", true);
	}
	spin_unlock_irqrestore(&c->lock, flags);
}

void mt7925_mlo_completion_init(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;

	/* MAC changes can remove/re-add the same vif. Do not reinitialize a
	 * spinlock observed by an atomic reset iterator or recycle old cookies.
	 */
	if (smp_load_acquire(&c->initialized))
		return;
	spin_lock_init(&c->lock);
	wiphy_work_init(&c->work, mt7925_mlo_completion_work);
	mt7925_mlo_recovery_init(vif);
	smp_store_release(&c->initialized, true);
}

void mt7925_mlo_completion_invalidate(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	unsigned long flags;

	if (!smp_load_acquire(&c->initialized))
		return;
	spin_lock_irqsave(&c->lock, flags);
	c->enabled = false;
	c->assoc_epoch++;
	mt7925_mlo_admit_fail(vif)->pending = false;
	c->result.phase = MT792X_MLO_CORE_IDLE;
	spin_unlock_irqrestore(&c->lock, flags);
	mt7925_mlo_recovery_assoc_lost(vif);
	mt7927_mlo_pairs_invalidate(vif, true);
}

void mt7925_mlo_completion_assoc(struct ieee80211_vif *vif, bool assoc)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	struct wiphy *wiphy = mt76_hw(mvif->phy->dev)->wiphy;
	unsigned long flags;

	lockdep_assert_wiphy(wiphy);
	mt7925_mlo_completion_invalidate(vif);
	wiphy_work_cancel(wiphy, &c->work);
	if (!assoc)
		return;
	spin_lock_irqsave(&c->lock, flags);
	c->enabled = true;
	spin_unlock_irqrestore(&c->lock, flags);
}

/* Teardown holds wiphy. Close producers and cancel owned wiphy work BEFORE
 * joining the ordinary validator; that worker only reads our record, never
 * waits for wiphy_lock or for a core worker that needs that lock.
 */
void mt7925_mlo_completion_stop(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;

	mt7925_mlo_completion_assoc(vif, false);
	mt7925_mlo_recovery_stop(vif);
	cancel_delayed_work_sync(&mvif->mlo_steer_work);
}

/* Atomic reset iterator: stop joins reset work under wiphy, so taking wiphy
 * or waiting here would invert that cancellation. Queued work sees disabled.
 */
void mt7925_mlo_completion_reset_iter(void *data, u8 *mac,
				     struct ieee80211_vif *vif)
{
	mt7925_mlo_completion_invalidate(vif);
}

static void mt7925_mlo_completion_foreign(struct ieee80211_vif *vif,
					bool keep_admit_fail)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	unsigned long flags;

	if (!smp_load_acquire(&c->initialized))
		return;
	spin_lock_irqsave(&c->lock, flags);
	if (!c->executing) {
		c->result.ticket.cookie = ++c->next_cookie;
		c->result.phase = MT792X_MLO_CORE_IDLE;
		if (!keep_admit_fail)
			mt7925_mlo_admit_fail(vif)->pending = false;
	}
	spin_unlock_irqrestore(&c->lock, flags);
	mt7927_mlo_pairs_invalidate(vif, false);
}

/* Foreign core/debugfs/key changes supersede validation, including ABA.
 * Callbacks within our synchronous outer operation are not foreign. A link
 * transition callback also cancels a pending failed-admission repair.
 */
void mt7925_mlo_completion_changed(struct ieee80211_vif *vif)
{
	mt7925_mlo_completion_foreign(vif, false);
}

/* Key callback. The incoming-GTK switch that mac80211 performs after a failed
 * replacement admission is part of the same outer setter, so it supersedes
 * validation but must not cancel the repair of that very setter. A later link
 * transition still does, through mt7925_mlo_completion_changed().
 */
void mt7925_mlo_completion_key_changed(struct ieee80211_vif *vif)
{
	mt7925_mlo_completion_foreign(vif, true);
}

/* External request submission is not a callback inside the owned setter.
 * It invalidates even a RUNNING ticket. executing remains set until the
 * old setter returns, so its callbacks cannot corrupt a replacement record.
 */
void mt7925_mlo_completion_supersede(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	unsigned long flags;

	if (!smp_load_acquire(&c->initialized))
		return;
	spin_lock_irqsave(&c->lock, flags);
	c->result.ticket.cookie = ++c->next_cookie;
	c->result.phase = MT792X_MLO_CORE_IDLE;
	mt7925_mlo_admit_fail(vif)->pending = false;
	spin_unlock_irqrestore(&c->lock, flags);
}

void mt7925_mlo_completion_note_error(struct ieee80211_vif *vif, int error)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	unsigned long flags;

	if (!error || !smp_load_acquire(&c->initialized))
		return;
	spin_lock_irqsave(&c->lock, flags);
	if (c->result.phase == MT792X_MLO_CORE_RUNNING &&
	    !c->result.firmware_error)
		c->result.firmware_error = error;
	spin_unlock_irqrestore(&c->lock, flags);
}

int mt7925_mlo_completion_request_after(struct ieee80211_vif *vif, u16 target,
				      const struct mt792x_mlo_ticket *predecessor,
				      struct mt792x_mlo_ticket *ticket)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&c->lock, flags);
	if (!c->enabled || !vif->cfg.assoc ||
	    !mt7925_mlo_completion_device_ready(mvif)) {
		ret = -ECANCELED;
		goto out;
	}
	if (c->executing) {
		ret = -EBUSY;
		goto out;
	}
	/* Rollback must atomically own the completed predecessor, including
	 * same-mask foreign changes and ABA between observation and submission.
	 */
	if (predecessor &&
	    (!mt7925_mlo_ticket_equal(predecessor, &c->result.ticket) ||
	     predecessor->assoc_epoch != c->assoc_epoch ||
	     c->result.phase != MT792X_MLO_CORE_DONE)) {
		ret = -ESTALE;
		goto out;
	}
	if (!target || target & ~ieee80211_vif_usable_links(vif)) {
		ret = -EINVAL;
		goto out;
	}
	mt7925_mlo_admit_fail(vif)->pending = false;
	*ticket = (struct mt792x_mlo_ticket) {
		.cookie = ++c->next_cookie, .assoc_epoch = c->assoc_epoch,
		.target = target, .expected_active = READ_ONCE(vif->active_links),
	};
	c->result = (struct mt792x_mlo_result) {
		.ticket = *ticket, .phase = MT792X_MLO_CORE_QUEUED,
		.core_ret = -EINPROGRESS,
	};
	/* Queue while the producer gate is locked: cancellation closes that
	 * gate before cancelling, so no producer can enqueue after cancellation.
	 */
	wiphy_work_queue(mt76_hw(mvif->phy->dev)->wiphy, &c->work);
out:
	spin_unlock_irqrestore(&c->lock, flags);
	return ret;
}

int mt7925_mlo_completion_request(struct ieee80211_vif *vif, u16 target,
				struct mt792x_mlo_ticket *ticket)
{
	return mt7925_mlo_completion_request_after(vif, target, NULL, ticket);
}

int mt7925_mlo_completion_result(struct ieee80211_vif *vif,
			       const struct mt792x_mlo_ticket *ticket,
			       struct mt792x_mlo_result *result)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&c->lock, flags);
	*result = c->result;
	if (!c->enabled || !vif->cfg.assoc ||
	    !mt7925_mlo_completion_device_ready(mvif))
		ret = -ECANCELED;
	else if (!mt7925_mlo_ticket_equal(ticket, &result->ticket) ||
		 ticket->assoc_epoch != c->assoc_epoch)
		ret = -ESTALE;
	else if (result->phase != MT792X_MLO_CORE_DONE)
		ret = -EINPROGRESS;
	else
		ret = 0; /* DONE is a lifecycle status; core_ret may be ANY errno. */
	spin_unlock_irqrestore(&c->lock, flags);
	return ret;
}

/* Datapath evidence is supplied separately by the validator; never infer it
 * from a mask, callback return, hardware key marker, or a successful MCU send.
 */
bool mt7925_mlo_completion_validate(struct ieee80211_vif *vif,
				    const struct mt792x_mlo_ticket *ticket,
				    u16 authenticated_links)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_mlo_completion *c = &mvif->mlo_completion;
	unsigned long flags;
	bool valid;

	spin_lock_irqsave(&c->lock, flags);
	valid = c->enabled && vif->cfg.assoc &&
		mt7925_mlo_completion_device_ready(mvif) &&
		mt7925_mlo_ticket_equal(ticket, &c->result.ticket) &&
		ticket->assoc_epoch == c->assoc_epoch &&
		c->result.phase == MT792X_MLO_CORE_DONE &&
		!c->result.core_ret && !c->result.firmware_error &&
		c->result.final_active == ticket->target &&
		READ_ONCE(vif->active_links) == ticket->target &&
		(authenticated_links & ticket->target) == ticket->target;
	if (valid)
		c->result.validated_links = authenticated_links & ticket->target;
	spin_unlock_irqrestore(&c->lock, flags);
	return valid;
}
