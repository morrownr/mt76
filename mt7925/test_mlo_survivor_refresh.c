/* Standalone contract test for the survivor-MLD-refresh gating logic. Build
 * and run:
 *   gcc -Wall -Wextra -Werror -o /tmp/test_mlo_survivor_refresh \
 *       test_mlo_survivor_refresh.c && /tmp/test_mlo_survivor_refresh
 */
#include <stdio.h>
#include "mlo_survivor_refresh.h"

#define L0 0x1UL
#define L1 0x2UL
#define L2 0x4UL

static int failures;

static void check(const char *name, unsigned long got, unsigned long want)
{
	if (got != want) {
		printf("FAIL %s: got=0x%lx want=0x%lx\n", name, got, want);
		failures++;
	} else {
		printf("PASS %s\n", name);
	}
}

static void checki(const char *name, int got, int want)
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
	/* CASE 1/2/3/4: survivor = old & new, per named transition. */
	check("survivor_0x6_to_0x3_is_link1",
	      mt7927_mlo_survivor_links(L1 | L2, L0 | L1), L1);
	check("survivor_0x3_to_0x5_is_link0",
	      mt7927_mlo_survivor_links(L0 | L1, L0 | L2), L0);
	check("survivor_0x5_to_0x6_is_link2",
	      mt7927_mlo_survivor_links(L0 | L2, L1 | L2), L2);

	/* CASE 5: initial association (old_links==0) -- no survivor. */
	check("no_refresh_on_initial_association",
	      mt7927_mlo_survivor_links(0, L0 | L1), 0);

	/* CASE 6: complete teardown (new_links==0) -- no survivor. */
	check("no_refresh_on_complete_teardown",
	      mt7927_mlo_survivor_links(L0 | L1, 0), 0);

	/* CASE 7: readiness gate refuses an intermediate union mask -- only
	 * the final published active_links may be serialized.
	 */
	checki("refuse_intermediate_union_mask",
	      mt7927_mlo_survivor_refresh_ready(L0 | L1 | L2, L0 | L1), 0);

	/* CASE 9: final active pair must equal intended new pair before
	 * serialization -- matching values are ready.
	 */
	checki("ready_when_active_links_matches_final",
	      mt7927_mlo_survivor_refresh_ready(L0 | L1, L0 | L1), 1);

	/* Disjoint sets (no actual survivor) still compute cleanly to 0,
	 * exercising the same code path a caller would hit for a pair with
	 * no overlap at all (never happens for real 2-of-3 link pairs, but
	 * the helper must not assume a nonempty intersection).
	 */
	check("disjoint_sets_no_survivor",
	      mt7927_mlo_survivor_links(L0, L1), 0);

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}
