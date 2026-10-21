/* Standalone contract test for mlo_steer_validate.h -- NOT part of the
 * kernel module build. Plain userspace C, no kernel headers.
 *
 * Build and run:
 *   gcc -Wall -Wextra -o /tmp/test_mlo_steer_validate test_mlo_steer_validate.c && /tmp/test_mlo_steer_validate
 */
#include <stdio.h>
#include "mlo_steer_validate.h"

#define MIN_PKTS 3
#define DEAD_TX 10
#define WINDOW_MS 2000

static int failures;

static void
check(const char *name, int got, int want)
{
	if (got != want) {
		printf("FAIL %s: got=%d want=%d\n", name, got, want);
		failures++;
	} else {
		printf("PASS %s\n", name);
	}
}

int main(void)
{
	/* Case 1: the exact real run -- new pair = link0+link1. link1 has
	 * real tx/rx progress (pass). link0 has tx=0/rx=0 because the
	 * deterministic scheduler never selected it (not because it's dead).
	 * Must NOT classify link0 as FAIL, and the pair must still commit.
	 */
	{
		enum mt7925_mlo_link_validate_result link1 =
			mt7925_mlo_classify_link_validation(59, 24, MIN_PKTS, DEAD_TX, 0, WINDOW_MS);
		enum mt7925_mlo_link_validate_result link0 =
			mt7925_mlo_classify_link_validation(0, 0, MIN_PKTS, DEAD_TX, 0, WINDOW_MS);
		int any_fail = (link0 == MT7925_LINK_VALIDATE_FAIL) ||
			       (link1 == MT7925_LINK_VALIDATE_FAIL);
		int any_pass = (link0 == MT7925_LINK_VALIDATE_PASS) ||
			       (link1 == MT7925_LINK_VALIDATE_PASS);

		check("case1_link0_not_classified_fail",
		     link0 == MT7925_LINK_VALIDATE_FAIL, 0);
		check("case1_link0_classified_deferred",
		     link0 == MT7925_LINK_VALIDATE_DEFERRED, 1);
		check("case1_pair_commits_despite_idle_link0",
		     mt7925_mlo_pair_validate_ok(any_fail, any_pass), 1);
	}

	/* Case 2: link0 deliberately exercised (tx well beyond threshold),
	 * rx stays zero -> real failure, must FAIL, and must block commit.
	 */
	{
		enum mt7925_mlo_link_validate_result link0 =
			mt7925_mlo_classify_link_validation(15, 0, MIN_PKTS, DEAD_TX, 0, WINDOW_MS);
		int any_fail = (link0 == MT7925_LINK_VALIDATE_FAIL);
		int any_pass = (link0 == MT7925_LINK_VALIDATE_PASS);

		check("case2_exercised_tx_only_is_fail",
		     link0 == MT7925_LINK_VALIDATE_FAIL, 1);
		check("case2_pair_does_not_commit_on_real_failure",
		     mt7925_mlo_pair_validate_ok(any_fail, any_pass), 0);
	}

	/* Case 3: link0 deliberately exercised, both tx and rx advance
	 * within the recent window -> PASS.
	 */
	{
		enum mt7925_mlo_link_validate_result link0 =
			mt7925_mlo_classify_link_validation(15, 15, MIN_PKTS, DEAD_TX, 0, WINDOW_MS);

		check("case3_exercised_both_directions_is_pass",
		     link0 == MT7925_LINK_VALIDATE_PASS, 1);
	}

	/* Case 4: one pair member carries real traffic successfully while
	 * the other is still unexercised/UNKNOWN -> pair forwarding remains
	 * valid; per-link readiness for the idle one stays unresolved
	 * (DEFERRED), not proven, but that must not block the pair.
	 */
	{
		enum mt7925_mlo_link_validate_result live_member =
			mt7925_mlo_classify_link_validation(40, 10, MIN_PKTS, DEAD_TX, 0, WINDOW_MS);
		enum mt7925_mlo_link_validate_result idle_member =
			mt7925_mlo_classify_link_validation(0, 0, MIN_PKTS, DEAD_TX, 0, WINDOW_MS);
		int any_fail = (live_member == MT7925_LINK_VALIDATE_FAIL) ||
			       (idle_member == MT7925_LINK_VALIDATE_FAIL);
		int any_pass = (live_member == MT7925_LINK_VALIDATE_PASS) ||
			       (idle_member == MT7925_LINK_VALIDATE_PASS);

		check("case4_idle_member_stays_deferred_not_proven",
		     idle_member == MT7925_LINK_VALIDATE_DEFERRED, 1);
		check("case4_pair_forwarding_valid_despite_unresolved_readiness",
		     mt7925_mlo_pair_validate_ok(any_fail, any_pass), 1);
	}

	/* Regression guard: a stale/late-arriving sample outside the recent
	 * window must not silently PASS on old evidence -- but old good
	 * evidence is not proof of failure either, so it lands DEFERRED
	 * (untrusted-for-now), not FAIL (proven-bad). Only real, fresh RX
	 * counts as proof; only real TX-without-any-RX counts as failure.
	 */
	{
		enum mt7925_mlo_link_validate_result stale =
			mt7925_mlo_classify_link_validation(20, 20, MIN_PKTS, DEAD_TX,
							    WINDOW_MS + 1, WINDOW_MS);

		check("stale_progress_outside_window_is_not_pass",
		     stale == MT7925_LINK_VALIDATE_PASS, 0);
		check("stale_progress_outside_window_is_deferred_not_fail",
		     stale == MT7925_LINK_VALIDATE_DEFERRED, 1);
	}

	/* Case 5 (new, proven live 2026-09-12): RX-only activity. The
	 * deterministic TX selector kept our own outbound traffic on the
	 * survivor link, but the AP still delivered real downlink (e.g. TCP
	 * ACKs, control traffic) to the freshly admitted link with zero TX
	 * on it. This is NOT the both-zero unexercised case, and it is NOT
	 * the TX-without-RX failure pattern -- it is real, unsolicited,
	 * correctly-attributed RX, which is the harder-to-fake proof of a
	 * genuinely working peer. Must PASS, not FAIL.
	 */
	{
		enum mt7925_mlo_link_validate_result rx_only =
			mt7925_mlo_classify_link_validation(0, 133, MIN_PKTS, DEAD_TX,
							    0, WINDOW_MS);

		check("case5_rx_only_activity_is_pass_not_fail",
		     rx_only == MT7925_LINK_VALIDATE_PASS, 1);
	}

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}
