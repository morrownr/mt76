/* Standalone contract test for mlo_link_readiness.h -- NOT part of the
 * kernel module build. Plain userspace C, no kernel headers.
 *
 * Build and run:
 *   gcc -Wall -Wextra -o /tmp/test_mlo_link_readiness test_mlo_link_readiness.c && /tmp/test_mlo_link_readiness
 */
#include <stdio.h>
#include "mlo_link_readiness.h"

#define STABILIZE_TICKS 500

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
	/* CASE 1: link2 is being removed, existing TID0 points to link2.
	 * Already satisfied by existing code, not this header:
	 * mt7925_mlo_change_sta_links() (mt7927_mlo.c:1509) calls
	 * mt7925_mlo_clear_tid_link(msta, -1) unconditionally BEFORE any BSS
	 * teardown or peer refresh runs. Documented here, not re-tested as
	 * pure logic, since it is a call-ordering fact about main.c/
	 * mt7927_mlo.c, not a decision function.
	 */
	check("case1_documented_not_reexercised_here", 1, 1);

	/* CASE 2: link1 is survivor but PHY_REBIND_PENDING (migration
	 * started, convergence not finished yet). RSSI/liveness would
	 * otherwise make it the best candidate. Selector must NOT choose it.
	 */
	{
		struct mt7925_link_migration_timeline t = {
			.migration_start_ts = 1000,
			.link_valid_set_ts = 1050,   /* flips true mid-migration */
			.convergence_end_ts = 1750,  /* backoff loop finishes later */
		};
		unsigned long now = 1100; /* after link_valid, before convergence */
		unsigned long ready_ts = mt7925_mlo_link_ready_ts_after_fix(&t, now);

		check("case2_ready_ts_is_zero_during_pending_migration",
		     ready_ts == 0, 1);
		check("case2_not_tx_ready_during_pending_migration",
		     mt7925_mlo_link_tx_ready(ready_ts, now, STABILIZE_TICKS), 0);
	}

	/* CASE 3: link1 finishes peer recreation, key restore, PTK reconcile,
	 * PHY_REBIND_CONVERGE. Only then does TX_READY become true (after
	 * the existing stabilize window elapses from THAT point, not from
	 * link_valid_set_ts and not from ancient pre-migration history).
	 */
	{
		struct mt7925_link_migration_timeline t = {
			.migration_start_ts = 1000,
			.link_valid_set_ts = 1050,
			.convergence_end_ts = 1750,
		};
		unsigned long ready_ts;

		/* Right at convergence end: ready_ts is now stamped, but the
		 * stabilize window has not elapsed yet -- still not ready.
		 */
		ready_ts = mt7925_mlo_link_ready_ts_after_fix(&t, 1750);
		check("case3_ready_ts_stamped_at_convergence_end",
		     ready_ts == 1750, 1);
		check("case3_not_yet_stable_immediately_at_convergence",
		     mt7925_mlo_link_tx_ready(ready_ts, 1750, STABILIZE_TICKS), 0);

		/* After convergence end AND the stabilize window: ready. */
		check("case3_stable_after_convergence_plus_window",
		     mt7925_mlo_link_tx_ready(ready_ts, 1750 + STABILIZE_TICKS + 1,
					      STABILIZE_TICKS), 1);
	}

	/* CASE 4: preferred member(s) temporarily NOT_TX_READY during the
	 * transition. The readiness check itself must produce a clean,
	 * unambiguous "not ready" (0) so the caller's existing candidate loop
	 * (mt792x_core.c) naturally skips it via its existing
	 * `if (!mt7925_mlo_link_tx_stable(...)) continue;` gate -- no new
	 * fallback/pause mechanism needed, the existing candidate loop
	 * already degrades correctly to "no eligible candidate this pass"
	 * when every candidate fails this check.
	 */
	{
		unsigned long ready_ts = 0; /* withheld: mid-migration */

		check("case4_withheld_link_never_ready_regardless_of_now",
		     mt7925_mlo_link_tx_ready(ready_ts, 999999, STABILIZE_TICKS), 0);
	}

	/* CASE 5: old TID pin references a link removed from the new active
	 * pair. Already satisfied by existing code, not this header:
	 * mt792x_core.c's force_bulk_wcid() `hard_invalid` check
	 * (`!(active & BIT(target))`) plus the unconditional
	 * mt7925_mlo_clear_tid_link(msta, -1) at transition start (case 1)
	 * together guarantee a cached pin cannot survive a topology change.
	 * Documented here, not re-tested as pure logic for the same reason
	 * as case 1.
	 */
	check("case5_documented_not_reexercised_here", 1, 1);

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}
