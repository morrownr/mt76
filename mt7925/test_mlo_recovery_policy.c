/* Standalone contract test for the MT7927 post-failed-admission recovery policy.
 *   gcc -Wall -Wextra -Werror -o /tmp/t test_mlo_recovery_policy.c && /tmp/t
 *
 * Evidence this encodes (hw, Oct 18): after a failed replacement admission the
 * survivor keeps transmitting while its security-valid RX never advances, a
 * full scan returns 0 BSS, and only a firmware reset clears it.
 */
#include <stdio.h>
#include <string.h>
#include "mlo_recovery_policy.h"

static int failures;

static void check(const char *name, long got, long want)
{
	if (got != want) {
		printf("FAIL %s: got=%ld want=%ld\n", name, got, want);
		failures++;
	} else {
		printf("PASS %s\n", name);
	}
}

#define NEVER (~0ULL)

static struct mt7925_mlo_recovery r;

static void arm(void)
{
	mt7925_mlo_recovery_arm(&r, 100, 1000, 1000, 7);
}

int main(void)
{
	/* A: before the window closes nothing is decided */
	arm();
	check("A_wait_inside_window",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 2000, 7, NEVER), MT7925_MLO_REC_WAIT);

	/* B: a single RX movement does not end the watch; only RX that keeps moving
	 * for HEALTHY_MS does (hardware: RX still arrived for ~1 s after the failed add,
	 * then stopped, and an early "healthy" left the radio dead for good) */
	arm();
	check("B_first_rx_movement_keeps_watching",
	      mt7925_mlo_recovery_step(&r, 101, 1500, 1000 + 1000, 7, NEVER), MT7925_MLO_REC_WAIT);
	check("B_still_armed_after_first_movement", r.armed, 1);
	check("B_sustained_rx_is_healthy",
	      mt7925_mlo_recovery_step(&r, 102, 2500, 1000 + MT7925_MLO_REC_HEALTHY_MS, 7, NEVER),
	      MT7925_MLO_REC_HEALTHY);
	check("B_healthy_disarms", r.armed, 0);
	check("B_disarmed_never_acts",
	      mt7925_mlo_recovery_step(&r, 100, 1500, 1000 + 99000, 7, NEVER), MT7925_MLO_REC_IDLE);

	/* C: TX flows, RX frozen past the window -> dead -> one reset */
	arm();
	check("C_dead_after_window",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 5000, 7, NEVER), MT7925_MLO_REC_RESET);
	check("C_one_reset_per_association", r.resets, 1);
	check("C_reset_disarms", r.armed, 0);
	arm();
	check("C_second_dead_same_epoch_is_refused",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 5000, 7, NEVER), MT7925_MLO_REC_BACKOFF);

	/* D: no TX evidence is inconclusive, never a reset; the watch gives up */
	memset(&r, 0, sizeof(r));
	arm();
	check("D_idle_link_is_not_dead",
	      mt7925_mlo_recovery_step(&r, 100, 1003, 1000 + 8000, 7, NEVER), MT7925_MLO_REC_WAIT);
	check("D_idle_link_gives_up_after_max_watch",
	      mt7925_mlo_recovery_step(&r, 100, 1003, 1000 + MT7925_MLO_REC_MAX_WATCH_MS, 7, NEVER),
	      MT7925_MLO_REC_GIVE_UP);
	check("D_give_up_disarms", r.armed, 0);

	/* E: a different association epoch never acts on an old watch */
	memset(&r, 0, sizeof(r));
	arm();
	check("E_epoch_change_cancels",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 5000, 8, NEVER), MT7925_MLO_REC_CANCEL);
	check("E_cancel_disarms", r.armed, 0);

	/* F: reset rate limit across associations */
	memset(&r, 0, sizeof(r));
	arm();
	check("F_recent_reset_blocks",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 5000, 7, MT7925_MLO_REC_MIN_INTERVAL_MS - 1),
	      MT7925_MLO_REC_BACKOFF);
	arm();
	check("F_old_enough_reset_allowed",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 5000, 7, MT7925_MLO_REC_MIN_INTERVAL_MS),
	      MT7925_MLO_REC_RESET);

	/* G: wrapping 32-bit counters are differences, not magnitudes */
	memset(&r, 0, sizeof(r));
	mt7925_mlo_recovery_arm(&r, 0xfffffff0u, 0xfffffff8u, 1000, 7);
	check("G_tx_wraps_still_counts_as_tx",
	      mt7925_mlo_recovery_step(&r, 0xfffffff0u, 0x00000008u, 1000 + 5000, 7, NEVER),
	      MT7925_MLO_REC_RESET);
	memset(&r, 0, sizeof(r));
	mt7925_mlo_recovery_arm(&r, 0xfffffff0u, 0xfffffff8u, 1000, 7);
	check("G_rx_wrap_is_progress",
	      mt7925_mlo_recovery_step(&r, 0x00000002u, 0x00000008u, 1000 + 5000, 7, NEVER),
	      MT7925_MLO_REC_WAIT);

	/* H: losing the association while armed is not an excuse to skip the reset:
	 * hardware showed the radio stays dead after the disassociation (full scan = 0
	 * BSS). If TX-without-RX was already seen for >= LOST_MIN, the loss counts. */
	memset(&r, 0, sizeof(r));
	arm();
	check("H_sample_inside_window_waits",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 3000, 7, NEVER), MT7925_MLO_REC_WAIT);
	check("H_loss_after_evidence_resets",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 4000, 8, NEVER), MT7925_MLO_REC_RESET);
	memset(&r, 0, sizeof(r));
	arm();
	mt7925_mlo_recovery_step(&r, 100, 1003, 1000 + 3000, 7, NEVER);
	check("H_loss_without_tx_evidence_cancels",
	      mt7925_mlo_recovery_step(&r, 100, 1003, 1000 + 4000, 8, NEVER), MT7925_MLO_REC_CANCEL);
	memset(&r, 0, sizeof(r));
	arm();
	mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 500, 7, NEVER);
	check("H_loss_before_min_age_cancels",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 1000, 8, NEVER), MT7925_MLO_REC_CANCEL);
	memset(&r, 0, sizeof(r));
	arm();
	mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 3000, 7, NEVER);
	check("H_loss_respects_rate_limit",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 4000, 8, MT7925_MLO_REC_MIN_INTERVAL_MS - 1),
	      MT7925_MLO_REC_BACKOFF);

	/* I: the once-per-association limit is per association, not forever */
	memset(&r, 0, sizeof(r));
	mt7925_mlo_recovery_arm(&r, 100, 1000, 1000, 7);
	check("I_first_assoc_resets",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000 + 5000, 7, NEVER), MT7925_MLO_REC_RESET);
	mt7925_mlo_recovery_arm(&r, 100, 1000, 900000, 7);
	check("I_same_assoc_still_refused",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 900000 + 5000, 7, NEVER), MT7925_MLO_REC_BACKOFF);
	mt7925_mlo_recovery_arm(&r, 100, 1000, 1000000, 9);
	check("I_new_assoc_may_reset_again",
	      mt7925_mlo_recovery_step(&r, 100, 1050, 1000000 + 5000, 9, NEVER), MT7925_MLO_REC_RESET);

	/* K: the observed failure: RX moves briefly, then stops while TX keeps flowing.
	 * The window restarts at the last RX movement, so the reset still fires. */
	memset(&r, 0, sizeof(r));
	arm();
	check("K_brief_rx_then_waits",
	      mt7925_mlo_recovery_step(&r, 101, 1010, 1000 + 1000, 7, NEVER), MT7925_MLO_REC_WAIT);
	check("K_window_counts_from_last_rx_movement",
	      mt7925_mlo_recovery_step(&r, 101, 1040, 1000 + 5000, 7, NEVER), MT7925_MLO_REC_WAIT);
	check("K_dead_after_window_from_last_rx",
	      mt7925_mlo_recovery_step(&r, 101, 1060, 1000 + 6000, 7, NEVER), MT7925_MLO_REC_RESET);

	/* L: RX that keeps moving across the healthy period ends the watch */
	memset(&r, 0, sizeof(r));
	arm();
	mt7925_mlo_recovery_step(&r, 101, 1010, 1000 + 2000, 7, NEVER);
	mt7925_mlo_recovery_step(&r, 102, 1020, 1000 + 6000, 7, NEVER);
	check("L_rx_moving_for_healthy_period",
	      mt7925_mlo_recovery_step(&r, 103, 1030, 1000 + MT7925_MLO_REC_HEALTHY_MS + 1000, 7, NEVER),
	      MT7925_MLO_REC_HEALTHY);

	/* M: a brief RX burst on an idle link (no TX) is never dead, and the watch gives up */
	memset(&r, 0, sizeof(r));
	arm();
	mt7925_mlo_recovery_step(&r, 101, 1000, 1000 + 2000, 7, NEVER);
	check("M_idle_after_burst_waits",
	      mt7925_mlo_recovery_step(&r, 101, 1003, 1000 + 9000, 7, NEVER), MT7925_MLO_REC_WAIT);
	check("M_idle_gives_up",
	      mt7925_mlo_recovery_step(&r, 101, 1003, 1000 + MT7925_MLO_REC_MAX_WATCH_MS, 7, NEVER),
	      MT7925_MLO_REC_GIVE_UP);

	/* N: a reassociation fallback (a failed grant with a healthy radio) is allowed
	 * the first time, then only after the minimum interval, in any association
	 */
	check("N_reassoc_first_time_allowed", mt7925_mlo_reassoc_allowed(NEVER), 1);
	check("N_reassoc_blocked_just_inside_interval",
	      mt7925_mlo_reassoc_allowed(MT7925_MLO_REASSOC_MIN_INTERVAL_MS - 1), 0);
	check("N_reassoc_allowed_at_interval",
	      mt7925_mlo_reassoc_allowed(MT7925_MLO_REASSOC_MIN_INTERVAL_MS), 1);
	check("N_reassoc_interval_is_shorter_than_reset_interval",
	      MT7925_MLO_REASSOC_MIN_INTERVAL_MS < MT7925_MLO_REC_MIN_INTERVAL_MS, 1);

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures ? 1 : 0;
}
