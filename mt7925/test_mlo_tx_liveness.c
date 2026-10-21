/* Standalone contract test for mlo_tx_liveness.h -- NOT part of the kernel
 * module build. Plain userspace C, no kernel headers.
 *
 * Build and run:
 *   gcc -Wall -Wextra -o /tmp/test_mlo_tx_liveness test_mlo_tx_liveness.c && /tmp/test_mlo_tx_liveness
 */
#include <stdio.h>
#include "mlo_tx_liveness.h"

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
	/* Case 1: link1 has strong RSSI but is DEAD (RX stalled despite TX
	 * progressing); link2 has weaker RSSI but is LIVE. link2 must win --
	 * a dead link must never beat a live one regardless of RSSI.
	 */
	{
		struct mt7925_tx_liveness_candidate link1 = {
			.liveness = MT7925_TX_LIVENESS_DEAD, .rssi = -50, .link_id = 1,
		};
		struct mt7925_tx_liveness_candidate link2 = {
			.liveness = MT7925_TX_LIVENESS_LIVE, .rssi = -65, .link_id = 2,
		};
		int link2_beats_link1 = mt7925_mlo_tx_candidate_better(&link2, &link1);
		int link1_beats_link2 = mt7925_mlo_tx_candidate_better(&link1, &link2);

		check("case1_link2_wins_over_dead_link1", link2_beats_link1, 1);
		check("case1_dead_link1_never_beats_live_link2", link1_beats_link2, 0);
	}

	/* Case 2: both LIVE, differ only by RSSI. Higher (less negative) RSSI
	 * wins -- link1 (-50) beats link2 (-65).
	 */
	{
		struct mt7925_tx_liveness_candidate link1 = {
			.liveness = MT7925_TX_LIVENESS_LIVE, .rssi = -50, .link_id = 1,
		};
		struct mt7925_tx_liveness_candidate link2 = {
			.liveness = MT7925_TX_LIVENESS_LIVE, .rssi = -65, .link_id = 2,
		};

		check("case2_stronger_rssi_wins_within_same_liveness",
		     mt7925_mlo_tx_candidate_better(&link1, &link2), 1);
		check("case2_weaker_rssi_does_not_win",
		     mt7925_mlo_tx_candidate_better(&link2, &link1), 0);
	}

	/* Case 3: no LIVE candidate anywhere -- CACHED must still beat
	 * UNKNOWN, and UNKNOWN must still beat DEAD, so the fallback path is
	 * always defined (never returns "no candidate at all" just because
	 * nothing is LIVE) and never permanently black-holes traffic onto a
	 * proven-dead link when literally anything else is available.
	 */
	{
		struct mt7925_tx_liveness_candidate cached = {
			.liveness = MT7925_TX_LIVENESS_CACHED, .rssi = -70, .link_id = 0,
		};
		struct mt7925_tx_liveness_candidate unknown = {
			.liveness = MT7925_TX_LIVENESS_UNKNOWN, .rssi = -40, .link_id = 1,
		};
		struct mt7925_tx_liveness_candidate dead = {
			.liveness = MT7925_TX_LIVENESS_DEAD, .rssi = -30, .link_id = 2,
		};

		check("case3_cached_beats_unknown_even_with_worse_rssi",
		     mt7925_mlo_tx_candidate_better(&cached, &unknown), 1);
		check("case3_unknown_beats_dead_even_with_worse_rssi",
		     mt7925_mlo_tx_candidate_better(&unknown, &dead), 1);
		check("case3_cached_beats_dead",
		     mt7925_mlo_tx_candidate_better(&cached, &dead), 1);
	}

	/* Tie-break: identical liveness and RSSI must deterministically
	 * prefer the lower link_id, not iteration order.
	 */
	{
		struct mt7925_tx_liveness_candidate a = {
			.liveness = MT7925_TX_LIVENESS_LIVE, .rssi = -60, .link_id = 0,
		};
		struct mt7925_tx_liveness_candidate b = {
			.liveness = MT7925_TX_LIVENESS_LIVE, .rssi = -60, .link_id = 2,
		};

		check("tie_break_lower_link_id_wins",
		     mt7925_mlo_tx_candidate_better(&a, &b), 1);
	}

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}
