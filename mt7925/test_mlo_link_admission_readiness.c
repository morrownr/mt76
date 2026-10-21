/* Standalone contract test for FRESH LINK ADMISSION readiness, reusing
 * mlo_link_readiness.h UNCHANGED -- the ordering contract is identical in
 * shape to the already-fixed migration case, so no new header is needed.
 * NOT part of the kernel module build. Plain userspace C, no kernel headers.
 *
 * Root cause this exists to fix (proven live, 2026-09-13, automatic
 * 0x6->0x3 switch at 05:08:11): a freshly-admitted link (link0, joining the
 * pair for the first time, distinct from a migrated survivor) completed
 * CREATE (STATE_NONE,newly=1) -> RESTORE (PTK) -> AUTHORIZE
 * (STATE_ASSOC,newly=0) -> MLO_LINK_READY correctly, in the right order.
 * But msta->mlo_link_ready_jiffies[link_id] is stamped ONCE, unconditionally,
 * at that same MLO_LINK_READY point (main.c) -- there is no bounded RX
 * convergence loop guarding it the way mt7927_mlo_refresh_reassigned_sta()'s
 * migration path already has. Live trace: real ARP + 8 real pings dispatched
 * to link0 within the 8s VERIFY window, rx_pkts_delta=0 the entire time,
 * resolved_link=0 appearing zero times in the kernel log. Steering
 * validation correctly refused to PASS (no false commit), but the exposure
 * to ordinary TX happened long before any RX evidence existed at all --
 * exactly the class of bug already fixed for migration, just missing here.
 *
 * Build and run:
 *   gcc -Wall -Wextra -o /tmp/test_mlo_link_admission_readiness \
 *       test_mlo_link_admission_readiness.c && \
 *       /tmp/test_mlo_link_admission_readiness
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

/* Models the CURRENT (pre-fix) fresh-admission source: ready_ts is stamped
 * unconditionally at MLO_LINK_READY time (creation-time), independent of
 * whether convergence has proven anything yet. This is what
 * mt7927_mlo_change_sta_links()'s add-loop does today -- no withhold, no
 * convergence gate.
 */
static unsigned long
current_source_ready_ts(unsigned long link_ready_ts)
{
	return link_ready_ts;
}

int main(void)
{
	/* CASE 1 + demonstrates the pre-fix bug: a newly admitted link that
	 * completed CREATE/RESTORE/AUTHORIZE but has NO RX progress yet must
	 * not be TX-ready. The CURRENT source violates this: it stamps
	 * ready_ts at admission time regardless of convergence, so the link
	 * reads "stable" the instant MT7925_MLO_TX_STABILIZE_MS elapses,
	 * with zero RX evidence -- proven live (8 real pings, 0 replies,
	 * for the entire 8s VERIFY window).
	 */
	{
		unsigned long link_ready_ts = 1000; /* MLO_LINK_READY fires */
		unsigned long now = link_ready_ts + STABILIZE_TICKS + 1;
		unsigned long pre_fix_ready_ts = current_source_ready_ts(link_ready_ts);

		check("case1_PREFIX_bug_wrongly_reports_ready_with_zero_rx",
		     mt7925_mlo_link_tx_ready(pre_fix_ready_ts, now, STABILIZE_TICKS), 1);
	}

	/* Same scenario, POST-FIX: ready_ts must stay 0 until convergence
	 * proves RX progress, reusing the exact migration model unchanged.
	 */
	{
		struct mt7925_link_migration_timeline t = {
			.migration_start_ts = 1000,   /* admission/CREATE starts */
			.link_valid_set_ts = 1050,    /* AUTHORIZE flips link_valid */
			.convergence_end_ts = 1750,   /* bounded backoff loop finishes */
		};
		unsigned long now = t.link_valid_set_ts + STABILIZE_TICKS + 1;
		unsigned long ready_ts = mt7925_mlo_link_ready_ts_after_fix(&t, now);

		check("case2_not_selector_eligible_from_link_valid_alone",
		     mt7925_mlo_link_tx_ready(ready_ts, now, STABILIZE_TICKS), 0);
	}

	/* CASE 3: one real RX progress event after admission is sufficient --
	 * modeled identically to migration: convergence_end_ts is reached
	 * exactly when rx_packets moved, whether on attempt 1 or attempt 4.
	 */
	{
		struct mt7925_link_migration_timeline t = {
			.migration_start_ts = 1000,
			.link_valid_set_ts = 1050,
			.convergence_end_ts = 1100, /* RX moved on the very first check */
		};
		unsigned long ready_ts = mt7925_mlo_link_ready_ts_after_fix(&t, 1100);

		check("case3_single_rx_progress_event_is_sufficient",
		     ready_ts == 1100, 1);
	}

	/* CASE 4: if convergence is not proven within the bounded retry
	 * window, readiness remains withheld for the whole window --
	 * checked at every point strictly before convergence_end_ts.
	 */
	{
		struct mt7925_link_migration_timeline t = {
			.migration_start_ts = 1000,
			.link_valid_set_ts = 1050,
			.convergence_end_ts = 1750,
		};
		unsigned long mid_window = 1500;

		check("case4_withheld_throughout_bounded_window",
		     mt7925_mlo_link_ready_ts_after_fix(&t, mid_window), 0);
	}

	/* CASE 5: failure of the new link's convergence must not affect a
	 * DIFFERENT link_id's independent timeline -- modeled by simply
	 * using a second, unrelated timeline instance and confirming no
	 * shared/global state is implied by the pure function signature
	 * (it takes only its own timeline, nothing else).
	 */
	{
		struct mt7925_link_migration_timeline failed_new_link = {
			.migration_start_ts = 1000,
			.link_valid_set_ts = 1050,
			.convergence_end_ts = 999999, /* never converges in practice */
		};
		struct mt7925_link_migration_timeline healthy_survivor = {
			.migration_start_ts = 1000,
			.link_valid_set_ts = 1010,
			.convergence_end_ts = 1020,
		};
		unsigned long now = 2000;

		check("case5_failed_new_link_stays_not_ready",
		     mt7925_mlo_link_ready_ts_after_fix(&failed_new_link, now) == 0, 1);
		check("case5_survivor_unaffected_by_new_link_failure",
		     mt7925_mlo_link_tx_ready(
			     mt7925_mlo_link_ready_ts_after_fix(&healthy_survivor, now),
			     now, STABILIZE_TICKS),
		     1);
	}

	/* CASE 6: topology independence. The model takes no link_id or pair
	 * mask at all -- link0, link1, and link2 all use the identical
	 * function with identical semantics. Prove this explicitly for all
	 * three by running the same timeline shape through each nominal
	 * link_id (represented here just as a label, since the pure logic
	 * never branches on it).
	 */
	{
		const char *labels[3] = { "link0", "link1", "link2" };
		int link_id;

		for (link_id = 0; link_id < 3; link_id++) {
			struct mt7925_link_migration_timeline t = {
				.migration_start_ts = 1000,
				.link_valid_set_ts = 1050,
				.convergence_end_ts = 1200,
			};
			char name[64];

			snprintf(name, sizeof(name),
				"case6_topology_independent_%s", labels[link_id]);
			check(name,
			     mt7925_mlo_link_ready_ts_after_fix(&t, 1150) == 0, 1);
		}
	}

	/* CASE 7 + 8: repeated admission generations do not inherit stale RX
	 * history. A prior generation's convergence_end_ts (long past) must
	 * not satisfy a NEW generation's admission -- each admission gets a
	 * fresh timeline instance; nothing carries over because the pure
	 * function has no persistent state, and the caller is responsible
	 * for building a brand-new timeline (with a fresh baseline) per
	 * admission. Model generation 1 converging, then generation 2
	 * starting fresh and NOT yet converged at the same wall-clock "now"
	 * a naive stale-timestamp reuse would have wrongly reported ready.
	 */
	{
		struct mt7925_link_migration_timeline gen1 = {
			.migration_start_ts = 1000,
			.link_valid_set_ts = 1010,
			.convergence_end_ts = 1020, /* gen1 converged quickly */
		};
		struct mt7925_link_migration_timeline gen2 = {
			.migration_start_ts = 5000, /* link removed and re-admitted */
			.link_valid_set_ts = 5010,
			.convergence_end_ts = 5800, /* gen2 has NOT converged yet */
		};
		unsigned long now = 5500; /* well past gen1's old convergence_end_ts */

		check("case7_gen1_converged_ready_at_its_own_time",
		     mt7925_mlo_link_ready_ts_after_fix(&gen1, 1020) == 1020, 1);
		check("case8_gen2_not_satisfied_by_gen1_stale_convergence",
		     mt7925_mlo_link_ready_ts_after_fix(&gen2, now) == 0, 1);
	}

	/* CASE 9: rearm happens only after successful convergence, never
	 * before -- already covered by cases 2-4, restated as a direct
	 * boundary check at convergence_end_ts - 1 vs convergence_end_ts.
	 */
	{
		struct mt7925_link_migration_timeline t = {
			.migration_start_ts = 1000,
			.link_valid_set_ts = 1050,
			.convergence_end_ts = 1750,
		};

		check("case9_not_ready_one_tick_before_convergence",
		     mt7925_mlo_link_ready_ts_after_fix(&t, 1749) == 0, 1);
		check("case9_ready_ts_stamped_exactly_at_convergence",
		     mt7925_mlo_link_ready_ts_after_fix(&t, 1750) == 1750, 1);
	}

	/* CASE 10: rollback/remove after failed convergence leaves the link
	 * not-ready -- a convergence_end_ts far in the future (never
	 * reached because the caller gave up / rolled back) means ready_ts
	 * stays 0 for any "now" the caller might still query afterward.
	 */
	{
		struct mt7925_link_migration_timeline never_converged = {
			.migration_start_ts = 1000,
			.link_valid_set_ts = 1050,
			.convergence_end_ts = (unsigned long)-1, /* never */
		};

		check("case10_rollback_after_failed_convergence_stays_not_ready",
		     mt7925_mlo_link_ready_ts_after_fix(&never_converged, 999999) == 0, 1);
	}

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}
