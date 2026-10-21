/* Standalone contract test for the sole-secure-link preservation rule.
 *   gcc -Wall -Wextra -Werror -o /tmp/t test_mlo_secure_shrink.c && /tmp/t
 */
#include <stdio.h>
#include "mlo_secure_shrink.h"

static int failures;

static void check(const char *name, int got, int want)
{
	if (got != want) {
		printf("FAIL %s: got=%d want=%d\n", name, got, want);
		failures++;
	} else {
		printf("PASS %s\n", name);
	}
}

#define V(o, n, s) mt7925_mlo_secure_transition_verdict(o, n, s)

int main(void)
{
	unsigned int o, n, s;
	int bad = 0;

	/* 2026-10-04: L2 (bit2) is the only secure link, 0x6 -> 0x3 forces survivor L1 */
	check("A_oct4_sole_secure_L2_departs_is_vetoed",
	      V(0x6, 0x3, 0x4), MT7925_MLO_SECURE_VETO_SECURE_DEPARTS);
	/* 2026-09-27: survivor is the secure link */
	check("B_sep27_survivor_secure_allowed",
	      V(0x6, 0x5, 0x4), MT7925_MLO_SECURE_ALLOW_SURVIVOR_SECURE);
	check("C_no_secure_link_allowed",
	      V(0x6, 0x3, 0x0), MT7925_MLO_SECURE_ALLOW_NO_SECURE_PATH);
	check("D_both_secure_survivor_secure_allowed",
	      V(0x6, 0x3, 0x6), MT7925_MLO_SECURE_ALLOW_SURVIVOR_SECURE);
	/* evidence for a link outside the current active set proves nothing */
	check("E_secure_bits_outside_old_active_ignored",
	      V(0x6, 0x3, 0x1), MT7925_MLO_SECURE_ALLOW_NO_SECURE_PATH);
	check("F_disjoint_masks_drop_sole_secure_link_vetoed",
	      V(0x6, 0x1, 0x2), MT7925_MLO_SECURE_VETO_SECURE_DEPARTS);
	check("G_single_link_moves_away_vetoed",
	      V(0x4, 0x1, 0x4), MT7925_MLO_SECURE_VETO_SECURE_DEPARTS);
	check("H_same_mask_is_noop",
	      V(0x6, 0x6, 0x4), MT7925_MLO_SECURE_ALLOW_NO_SECURE_PATH);

	/* topology independence: veto iff some old link is secure and no
	 * secure old link survives, for every old/new/secure combination */
	for (o = 1; o < 8; o++)
		for (n = 1; n < 8; n++)
			for (s = 0; s < 8; s++) {
				int want = !(o == n) && (o & s) && !((o & n) & (o & s));
				int got = !mt7925_mlo_secure_verdict_allows(V(o, n, s));

				if (got != want) {
					printf("FAIL I_exhaustive o=%x n=%x s=%x got=%d want=%d\n",
					       o, n, s, got, want);
					bad++;
				}
			}
	check("I_exhaustive_all_topologies", bad, 0);

	/* the six directed pair transitions with exactly one secure link */
	check("J_0x6_to_0x5_secure_L2", mt7925_mlo_secure_verdict_allows(V(0x6, 0x5, 0x4)), 1);
	check("J_0x5_to_0x3_secure_L2_vetoed", mt7925_mlo_secure_verdict_allows(V(0x5, 0x3, 0x4)), 0);
	check("J_0x5_to_0x3_secure_L0", mt7925_mlo_secure_verdict_allows(V(0x5, 0x3, 0x1)), 1);
	check("J_0x3_to_0x6_secure_L1", mt7925_mlo_secure_verdict_allows(V(0x3, 0x6, 0x2)), 1);
	check("J_0x3_to_0x5_secure_L1_vetoed", mt7925_mlo_secure_verdict_allows(V(0x3, 0x5, 0x2)), 0);
	check("J_0x6_to_0x3_secure_L1", mt7925_mlo_secure_verdict_allows(V(0x6, 0x3, 0x2)), 1);

	/* delayed rollback failure: host mask alone is not restoration */
	check("K_mask_match_without_secure_rx_not_restored",
	      mt7925_mlo_rollback_restored(0x6, 0x6, 0x0), 0);
	check("L_secure_rx_but_mask_not_restored",
	      mt7925_mlo_rollback_restored(0x2, 0x6, 0x2), 0);
	check("M_mask_and_secure_rx_restored",
	      mt7925_mlo_rollback_restored(0x6, 0x6, 0x4), 1);
	check("N_secure_rx_only_on_non_target_link_not_restored",
	      mt7925_mlo_rollback_restored(0x6, 0x6, 0x1), 0);

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures != 0;
}
