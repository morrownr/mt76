/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The runner extracts the actual production completion functions here. The
 * setter, key switching and wiphy work scheduling below are real mac80211.
 * Driver callbacks remain the explicit transport-boundary doubles in
 * mlo_activation.c. No observer injects a production completion notification.
 *
 * Breaks caught: treating a published target as completion; losing the outer
 * return; accepting an older cookie/association after ABA; running canceled
 * work against freed state; or waiting for a blocked grow while holding the
 * lock needed by that operation. Firmware/MCU and real RX are not modeled.
 */
#include "mlo_completion_body.inc"

static int mlo_completion_flush_result(struct mlo_fixture *f,
				     const struct mt792x_mlo_ticket *ticket,
				     struct mt792x_mlo_result *result)
{
	int ret;

	/* A real grow callback may hold this lock. Release its test barrier first. */
	complete_all(&f->grow_release);
	wiphy_lock(f->hw->wiphy);
	wiphy_work_flush(f->hw->wiphy, NULL);
	ret = mt7925_mlo_completion_result(&f->sdata->vif, ticket, result);
	if (!ret)
		ret = result->core_ret;
	wiphy_unlock(f->hw->wiphy);
	return ret;
}

static void mlo_completion_forward_waits_for_outer_and_keys(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;
	bool entered;
	int i, key_sets = 0;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	f->block_new = 5;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &ticket), 0);
	entered = wait_for_completion_timeout(&f->grow_entered, 2 * HZ);
	KUNIT_EXPECT_TRUE(test, entered);
	if (entered) {
		KUNIT_EXPECT_EQ(test, READ_ONCE(f->sdata->vif.active_links), (u16)5);
		KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
							&ticket, &result), -EINPROGRESS);
		for (i = 0; i < f->n_events; i++)
			key_sets += f->events[i].kind == MLO_KEY_SET;
		KUNIT_EXPECT_EQ(test, key_sets, 0);
	}
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result), 0);
	KUNIT_EXPECT_EQ(test, result.core_ret, 0);
	KUNIT_EXPECT_EQ(test, result.final_active, (u16)5);
	key_sets = 0;
	for (i = 0; i < f->n_events; i++)
		if (f->events[i].kind == MLO_KEY_SET && f->events[i].link == 0)
			key_sets++;
	KUNIT_EXPECT_EQ(test, key_sets, 1);
}

static void mlo_completion_rollback_target_and_rx_do_not_finish_grow(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;
	bool entered;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->block_new = 6;
	f->fail_new = 6;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							6, &ticket), 0);
	entered = wait_for_completion_timeout(&f->grow_entered, 2 * HZ);
	KUNIT_EXPECT_TRUE(test, entered);
	if (entered) {
		KUNIT_EXPECT_EQ(test, READ_ONCE(f->sdata->vif.active_links), (u16)6);
		/* Even a declared fresh survivor sample is not outer completion. */
		f->peer[2]->mlo_valid_rx_packets++;
		WRITE_ONCE(f->peer[2]->mlo_valid_rx_jiffies, jiffies + 1);
		KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
							&ticket, &result), -EINPROGRESS);
	}
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result), -EIO);
	KUNIT_EXPECT_EQ(test, result.core_ret, -EIO);
	KUNIT_EXPECT_EQ(test, result.final_active, (u16)4);
}

static void mlo_completion_reports_core_refusal(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->refuse_new = 6;
	/* Driver queue acceptance is not can_activate_links/core acceptance. */
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							6, &ticket), 0);
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, result.core_ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, result.final_active, (u16)5);
	KUNIT_EXPECT_EQ(test, f->n_events, 0U);
}

static void mlo_completion_retry_after_partial_migration(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket failed, retry;
	struct mt792x_mlo_result result;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	f->fail_new = 5;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &failed), 0);
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &failed, &result), -EIO);
	KUNIT_EXPECT_EQ(test, result.final_active, (u16)4);
	f->fail_new = 0;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &retry), 0);
	KUNIT_EXPECT_NE(test, retry.cookie, failed.cookie);
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &retry, &result), 0);
	KUNIT_EXPECT_EQ(test, result.final_active, (u16)5);
	KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
							&failed, &result), -ESTALE);
}

static void mlo_completion_core_zero_does_not_hide_key_failure(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	f->fail_key_link = 0;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &ticket), 0);
	/* mac80211 can finish with software fallback after driver SET_KEY fails. */
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result), 0);
	KUNIT_EXPECT_EQ(test, result.core_ret, 0);
	KUNIT_EXPECT_EQ(test, result.final_active, (u16)5);
	KUNIT_EXPECT_EQ(test, result.firmware_error, -EIO);
	KUNIT_EXPECT_EQ(test, result.validated_links, (u16)0);
	f->fail_key_link = -1;
}

static void mlo_completion_queued_supersession_aba(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket first, middle, last;
	struct mt792x_mlo_result result;
	int accepted[3], first_status, middle_status, last_status;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	/* Hold the real serialization lock so all three requests precede execution. */
	wiphy_lock(f->hw->wiphy);
	accepted[0] = mt7925_mlo_completion_request(&f->sdata->vif, 5, &first);
	accepted[1] = mt7925_mlo_completion_request(&f->sdata->vif, 3, &middle);
	accepted[2] = mt7925_mlo_completion_request(&f->sdata->vif, 5, &last);
	first_status = mt7925_mlo_completion_result(&f->sdata->vif, &first, &result);
	middle_status = mt7925_mlo_completion_result(&f->sdata->vif, &middle, &result);
	last_status = mt7925_mlo_completion_result(&f->sdata->vif, &last, &result);
	wiphy_work_flush(f->hw->wiphy, NULL);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_ASSERT_EQ(test, accepted[0], 0);
	KUNIT_ASSERT_EQ(test, accepted[1], 0);
	KUNIT_ASSERT_EQ(test, accepted[2], 0);
	KUNIT_EXPECT_NE(test, first.cookie, last.cookie);
	KUNIT_EXPECT_EQ(test, first.target, last.target);
	KUNIT_EXPECT_EQ(test, first_status, -ESTALE);
	KUNIT_EXPECT_EQ(test, middle_status, -ESTALE);
	KUNIT_EXPECT_EQ(test, last_status, -EINPROGRESS);
	KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
							&last, &result), 0);
	KUNIT_EXPECT_EQ(test, result.final_active, (u16)5);
}

static void mlo_completion_association_epoch_rejects_old_result(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket old, fresh;
	struct mt792x_mlo_result result;
	int disabled, stale;

	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &old), 0);
	KUNIT_ASSERT_EQ(test, mlo_completion_flush_result(f, &old, &result), 0);
	wiphy_lock(f->hw->wiphy);
	mt7925_mlo_completion_assoc(&f->sdata->vif, false);
	disabled = mt7925_mlo_completion_result(&f->sdata->vif, &old, &result);
	mt7925_mlo_completion_assoc(&f->sdata->vif, true);
	stale = mt7925_mlo_completion_result(&f->sdata->vif, &old, &result);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, disabled, -ECANCELED);
	KUNIT_EXPECT_EQ(test, stale, -ESTALE);
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							6, &fresh), 0);
	KUNIT_EXPECT_NE(test, fresh.assoc_epoch, old.assoc_epoch);
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &fresh, &result), 0);
}

static void mlo_completion_foreign_callback_invalidates_queued_request(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;
	int accepted, changed, stale;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	wiphy_lock(f->hw->wiphy);
	accepted = mt7925_mlo_completion_request(&f->sdata->vif, 5, &ticket);
	/* A genuine unrelated core change invokes the recorder driver callback. */
	changed = ieee80211_set_active_links(&f->sdata->vif, 3);
	stale = mt7925_mlo_completion_result(&f->sdata->vif, &ticket, &result);
	wiphy_work_flush(f->hw->wiphy, NULL);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, accepted, 0);
	KUNIT_EXPECT_EQ(test, changed, 0);
	KUNIT_EXPECT_EQ(test, stale, -ESTALE);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)3);
}

static void mlo_completion_invalidation_prevents_late_running_publication(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;
	bool entered;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	f->block_new = 5;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &ticket), 0);
	entered = wait_for_completion_timeout(&f->grow_entered, 2 * HZ);
	KUNIT_EXPECT_TRUE(test, entered);
	if (entered) {
		/* Reset-side invalidation must not need the running worker's wiphy. */
		mt7925_mlo_completion_invalidate(&f->sdata->vif);
		KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
							&ticket, &result), -ECANCELED);
	}
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result),
			-ECANCELED);
	/* The old worker may return normally, but cannot revive the old epoch. */
	wiphy_lock(f->hw->wiphy);
	mt7925_mlo_completion_assoc(&f->sdata->vif, true);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
							&ticket, &result), -ESTALE);
}

static void mlo_completion_queued_teardown_cancels_without_wait(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket, rejected;
	struct mt792x_mlo_result result;
	int accepted, canceled, refused;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	wiphy_lock(f->hw->wiphy);
	accepted = mt7925_mlo_completion_request(&f->sdata->vif, 5, &ticket);
	/* Same cancellation context as interface teardown: wiphy is held. */
	mt7925_mlo_completion_assoc(&f->sdata->vif, false);
	canceled = mt7925_mlo_completion_result(&f->sdata->vif, &ticket, &result);
	refused = mt7925_mlo_completion_request(&f->sdata->vif, 3, &rejected);
	wiphy_work_flush(f->hw->wiphy, NULL);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, accepted, 0);
	KUNIT_EXPECT_EQ(test, canceled, -ECANCELED);
	KUNIT_EXPECT_EQ(test, refused, -ECANCELED);
	KUNIT_EXPECT_EQ(test, f->n_events, 0U);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)6);
}

struct mlo_completion_cancel_thread {
	struct mlo_fixture *f;
	struct completion started, done;
};

static int mlo_completion_cancel_thread_fn(void *data)
{
	struct mlo_completion_cancel_thread *ctx = data;

	complete(&ctx->started);
	/* No worker wait under wiphy_lock: lock serializes before cancellation. */
	wiphy_lock(ctx->f->hw->wiphy);
	mt7925_mlo_completion_assoc(&ctx->f->sdata->vif, false);
	wiphy_unlock(ctx->f->hw->wiphy);
	complete(&ctx->done);
	/* Keep the task alive until the owner joins it. kthread_stop() cannot
	 * safely dereference an unreferenced task that already exited.
	 */
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void mlo_completion_running_teardown_has_no_lock_inversion(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mlo_completion_cancel_thread ctx = { .f = f };
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;
	struct task_struct *thread;
	bool entered, started = false, done = false;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	f->block_new = 5;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &ticket), 0);
	entered = wait_for_completion_timeout(&f->grow_entered, 2 * HZ);
	KUNIT_EXPECT_TRUE(test, entered);
	init_completion(&ctx.started);
	init_completion(&ctx.done);
	thread = kthread_run(mlo_completion_cancel_thread_fn, &ctx,
			     "mlo-cancel-kunit");
	if (!IS_ERR(thread)) {
		started = wait_for_completion_timeout(&ctx.started, 2 * HZ);
		KUNIT_EXPECT_FALSE(test, completion_done(&ctx.done));
	}
	/* Release before joining or attempting the wiphy lock, including errors. */
	complete_all(&f->grow_release);
	if (!IS_ERR(thread)) {
		done = wait_for_completion_timeout(&ctx.done, 3 * HZ);
		kthread_stop(thread);
	}
	KUNIT_EXPECT_FALSE(test, IS_ERR(thread));
	KUNIT_EXPECT_TRUE(test, started);
	KUNIT_EXPECT_TRUE(test, done);
	KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
							&ticket, &result), -ECANCELED);
}

static void mlo_completion_records_swallowed_sta_failure(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	f->fail_sta_new = 5;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &ticket), 0);
	/* Real core WARNs but does not return the STA callback error. */
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result), 0);
	KUNIT_EXPECT_EQ(test, result.firmware_error, -EIO);
	KUNIT_EXPECT_FALSE(test, mt7925_mlo_completion_validate(&f->sdata->vif,
							      &ticket, 5));
	f->fail_sta_new = 0;
}

/* Missing repair publishes 0x3 after no_secure_rx even though core returns 0.
 * The real core must finish its incoming GTK install before the same owner
 * invokes the genuine setter to remove the failed incoming link again.
 */
static void mlo_completion_failed_admission_restores_survivor(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;
	int i, incoming_key = -1, repair_key = -1;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->fail_admission_new = 3;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							3, &ticket), 0);
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result), 0);
	KUNIT_EXPECT_EQ(test, result.core_ret, 0);
	KUNIT_EXPECT_EQ(test, result.firmware_error, -ENODATA);
	KUNIT_EXPECT_EQ(test, result.final_active, (u16)1);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)1);
	/* The mask is repaired, not the radio: exactly one watch on the survivor. */
	KUNIT_EXPECT_EQ(test, f->rec_armed, 1u);
	KUNIT_EXPECT_EQ(test, f->rec_survivor, (u16)1);
	KUNIT_EXPECT_EQ(test, f->rec_failed, (u16)3);	/* the target that failed */
	KUNIT_EXPECT_EQ(test, f->rec_prev, (u16)5);	/* the connection being left */
	KUNIT_EXPECT_FALSE(test, mt7925_mlo_completion_validate(&f->sdata->vif,
							      &ticket, 3));
	for (i = 0; i < f->n_events; i++) {
		if (f->events[i].kind == MLO_KEY_SET && f->events[i].link == 1)
			incoming_key = i;
		if (f->events[i].kind == MLO_KEY_DISABLE && f->events[i].link == 1)
			repair_key = i;
	}
	KUNIT_EXPECT_GE(test, incoming_key, 0);
	KUNIT_EXPECT_GT(test, repair_key, incoming_key);
}

static void mlo_completion_async_failed_admission_restores_survivor(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->fail_admission_new = 3;
	mlo_submit_async(f, 3);
	KUNIT_ASSERT_TRUE(test, mlo_wait_outer(f));
	wiphy_lock(f->hw->wiphy);
	wiphy_work_flush(f->hw->wiphy, NULL);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, f->outer_ret, 0);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)1);
	KUNIT_EXPECT_EQ(test, f->rec_armed, 1u);
	KUNIT_EXPECT_EQ(test, f->rec_survivor, (u16)1);
}

static void mlo_completion_foreign_supersede_cancels_admission_repair(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	int ret;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->fail_admission_new = 3;
	wiphy_lock(f->hw->wiphy);
	ret = ieee80211_set_active_links(&f->sdata->vif, 3);
	/* No worker can run until the actual foreign outer setter returns. */
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)3);
	mt7925_mlo_completion_supersede(&f->sdata->vif);
	wiphy_work_flush(f->hw->wiphy, NULL);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)3);
	KUNIT_EXPECT_EQ(test, f->rec_armed, 0u);	/* nothing was repaired */
}

static void mlo_completion_new_association_cancels_admission_repair(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	int ret;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->fail_admission_new = 3;
	wiphy_lock(f->hw->wiphy);
	ret = ieee80211_set_active_links(&f->sdata->vif, 3);
	mt7925_mlo_completion_assoc(&f->sdata->vif, false);
	mt7925_mlo_completion_assoc(&f->sdata->vif, true);
	wiphy_work_flush(f->hw->wiphy, NULL);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)3);
	KUNIT_EXPECT_EQ(test, f->rec_armed, 0u);
	/* losing the association hands an outstanding watch its final evaluation */
	KUNIT_EXPECT_GT(test, f->rec_cancelled, 0u);
}

static void mlo_completion_foreign_aba_cancels_admission_repair(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	int failed, away, back;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->fail_admission_new = 3;
	wiphy_lock(f->hw->wiphy);
	failed = ieee80211_set_active_links(&f->sdata->vif, 3);
	f->fail_admission_new = 0;
	/* A newer pair operation returning to 0x3 is not the failed operation. */
	away = ieee80211_set_active_links(&f->sdata->vif, 5);
	back = ieee80211_set_active_links(&f->sdata->vif, 3);
	wiphy_work_flush(f->hw->wiphy, NULL);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, failed, 0);
	KUNIT_EXPECT_EQ(test, away, 0);
	KUNIT_EXPECT_EQ(test, back, 0);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)3);
}

static void mlo_completion_owned_request_supersedes_admission_repair(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;
	int failed, accepted;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 5), 0);
	f->fail_admission_new = 3;
	wiphy_lock(f->hw->wiphy);
	failed = ieee80211_set_active_links(&f->sdata->vif, 3);
	accepted = mt7925_mlo_completion_request(&f->sdata->vif, 3, &ticket);
	wiphy_work_flush(f->hw->wiphy, NULL);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, failed, 0);
	KUNIT_ASSERT_EQ(test, accepted, 0);
	KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
							&ticket, &result), 0);
	KUNIT_EXPECT_EQ(test, result.firmware_error, 0);
	KUNIT_EXPECT_EQ(test, result.final_active, (u16)3);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)3);
}

static void mlo_completion_running_submission_is_refused(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket, replacement;
	struct mt792x_mlo_result result;
	bool entered;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	f->block_new = 5;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &ticket), 0);
	entered = wait_for_completion_timeout(&f->grow_entered, 2 * HZ);
	KUNIT_EXPECT_TRUE(test, entered);
	if (entered)
		KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							3, &replacement), -EBUSY);
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result), 0);
}

static void mlo_completion_external_submission_supersedes_running(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket, replacement;
	struct mt792x_mlo_result result;
	bool entered;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	f->block_new = 5;
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &ticket), 0);
	entered = wait_for_completion_timeout(&f->grow_entered, 2 * HZ);
	KUNIT_EXPECT_TRUE(test, entered);
	if (entered) {
		mt7925_mlo_completion_supersede(&f->sdata->vif);
		KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
							&ticket, &result), -ESTALE);
		KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							3, &replacement), -EBUSY);
	}
	KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result), -ESTALE);
}

static void mlo_completion_rollback_predecessor_check_is_atomic(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	struct mt792x_mlo_ticket ticket, rollback;
	struct mt792x_mlo_result result;
	int ret;

	KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
	KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
							5, &ticket), 0);
	KUNIT_ASSERT_EQ(test, mlo_completion_flush_result(f, &ticket, &result), 0);
	/* Same-mask foreign policy change AFTER observation, BEFORE rollback. */
	mt7925_mlo_completion_supersede(&f->sdata->vif);
	wiphy_lock(f->hw->wiphy);
	ret = mt7925_mlo_completion_request_after(&f->sdata->vif, 6,
						 &ticket, &rollback);
	wiphy_unlock(f->hw->wiphy);
	KUNIT_EXPECT_EQ(test, ret, -ESTALE);
	KUNIT_EXPECT_EQ(test, f->sdata->vif.active_links, (u16)5);
}

static void mlo_completion_done_status_preserves_any_core_errno(struct kunit *test)
{
	struct mlo_fixture *f = test->priv;
	const int failures[] = { -EINPROGRESS, -ECANCELED, -ESTALE };
	struct mt792x_mlo_ticket ticket;
	struct mt792x_mlo_result result;
	int i;

	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		KUNIT_ASSERT_EQ(test, mlo_seed_active(f, 6), 0);
		f->fail_new = 5;
		f->fail_errno = failures[i];
		KUNIT_ASSERT_EQ(test, mt7925_mlo_completion_request(&f->sdata->vif,
								5, &ticket), 0);
		KUNIT_EXPECT_EQ(test, mlo_completion_flush_result(f, &ticket, &result),
				failures[i]);
		KUNIT_EXPECT_EQ(test, mt7925_mlo_completion_result(&f->sdata->vif,
								&ticket, &result), 0);
		KUNIT_EXPECT_EQ(test, result.phase, MT792X_MLO_CORE_DONE);
		KUNIT_EXPECT_EQ(test, result.core_ret, failures[i]);
		KUNIT_EXPECT_FALSE(test, mt7925_mlo_completion_validate(&f->sdata->vif,
								      &ticket, 5));
		f->fail_new = 0;
	}
}

static struct kunit_case mlo_completion_cases[] = {
	KUNIT_CASE(mlo_completion_forward_waits_for_outer_and_keys),
	KUNIT_CASE(mlo_completion_rollback_target_and_rx_do_not_finish_grow),
	KUNIT_CASE(mlo_completion_reports_core_refusal),
	KUNIT_CASE(mlo_completion_retry_after_partial_migration),
	KUNIT_CASE(mlo_completion_core_zero_does_not_hide_key_failure),
	KUNIT_CASE(mlo_completion_records_swallowed_sta_failure),
	KUNIT_CASE(mlo_completion_failed_admission_restores_survivor),
	KUNIT_CASE(mlo_completion_async_failed_admission_restores_survivor),
	KUNIT_CASE(mlo_completion_foreign_supersede_cancels_admission_repair),
	KUNIT_CASE(mlo_completion_new_association_cancels_admission_repair),
	KUNIT_CASE(mlo_completion_foreign_aba_cancels_admission_repair),
	KUNIT_CASE(mlo_completion_owned_request_supersedes_admission_repair),
	KUNIT_CASE(mlo_completion_running_submission_is_refused),
	KUNIT_CASE(mlo_completion_external_submission_supersedes_running),
	KUNIT_CASE(mlo_completion_rollback_predecessor_check_is_atomic),
	KUNIT_CASE(mlo_completion_done_status_preserves_any_core_errno),
	KUNIT_CASE(mlo_completion_queued_supersession_aba),
	KUNIT_CASE(mlo_completion_association_epoch_rejects_old_result),
	KUNIT_CASE(mlo_completion_foreign_callback_invalidates_queued_request),
	KUNIT_CASE(mlo_completion_invalidation_prevents_late_running_publication),
	KUNIT_CASE(mlo_completion_queued_teardown_cancels_without_wait),
	KUNIT_CASE(mlo_completion_running_teardown_has_no_lock_inversion),
	{}
};

static struct kunit_suite mlo_completion_suite = {
	.name = "mlo_activation_completion",
	.init = mlo_fixture_init,
	.exit = mlo_fixture_exit,
	.test_cases = mlo_completion_cases,
};
kunit_test_suite(mlo_completion_suite);
