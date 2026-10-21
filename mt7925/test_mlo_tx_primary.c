/* Standalone contract test for mlo_tx_primary.h -- NOT part of the kernel module build.
 *   gcc -Wall -Wextra -Werror -o /tmp/test_mlo_tx_primary test_mlo_tx_primary.c && /tmp/test_mlo_tx_primary
 *
 * Why the policy exists (Oct 13 2026, hardware): the per-WCID UWTBL TX PN counters of an MLD's links are NOT shared. With the
 * secondary WCID in the TXD the AP drops frames whose PN is below the highest PN already seen from the other link: forced
 * secondary pings were lost exactly until its PN counter passed the primary's (27 lost, PN gap 27). Frames addressed with the
 * primary WCID are striped across both radios by the firmware with one PN space and carried data on both links.
 */
#include <stdio.h>
#include "mlo_tx_primary.h"

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

int main(void)
{
	/* deflink active and valid: always the primary */
	check("A_deflink_active_valid", mt7925_mlo_tx_primary_link(1, 0x6, 0x7), 1);
	check("A2_deflink_is_secondary_id_but_primary", mt7925_mlo_tx_primary_link(2, 0x6, 0x7), 2);

	/* deflink left the active pair: lowest active valid link */
	check("B_deflink_not_active", mt7925_mlo_tx_primary_link(1, 0x5, 0x7), 0);
	check("B2_deflink_not_active_pair_12", mt7925_mlo_tx_primary_link(0, 0x6, 0x7), 1);

	/* deflink active but its WCID is not valid: next valid active link */
	check("C_deflink_invalid", mt7925_mlo_tx_primary_link(1, 0x6, 0x5), 2);

	/* nothing usable */
	check("D_no_valid_active", mt7925_mlo_tx_primary_link(1, 0x6, 0x1), -1);
	check("D2_empty_active", mt7925_mlo_tx_primary_link(1, 0x0, 0x7), -1);

	/* an inactive link is never returned even if its WCID is valid */
	check("E_inactive_valid_never_chosen", mt7925_mlo_tx_primary_link(1, 0x4, 0x7), 2);

	/* unspecified / out-of-range deflink id */
	check("F_deflink_unspecified", mt7925_mlo_tx_primary_link(15, 0x6, 0x7), 1);

	/* anchor stability: while the deflink stays active+valid, toggling the other link never moves the primary */
	{
		int ok = 1;
		unsigned int partner, act;

		for (partner = 0; partner < 3; partner++) {
			if (partner == 1)
				continue;
			act = (1u << 1) | (1u << partner);
			if (mt7925_mlo_tx_primary_link(1, act, 0x7) != 1)
				ok = 0;
			if (mt7925_mlo_tx_primary_link(1, 1u << 1, 0x7) != 1)
				ok = 0;
		}
		check("G_anchor_never_moves_while_active", ok, 1);
	}

	/* all six directed pair transitions, every deflink choice: result is in the target pair and valid */
	{
		static const unsigned int pairs[3] = { 0x6, 0x5, 0x3 };
		int ok = 1, i, j;
		unsigned int dl;

		for (i = 0; i < 3; i++)
			for (j = 0; j < 3; j++) {
				if (i == j)
					continue;
				for (dl = 0; dl < 3; dl++) {
					int r = mt7925_mlo_tx_primary_link(dl, pairs[j], 0x7);

					if (r < 0 || !(pairs[j] & (1u << r)))
						ok = 0;
					/* survivor-or-deflink stays primary when it is in the target pair */
					if ((pairs[j] & (1u << dl)) && r != (int)dl)
						ok = 0;
				}
			}
		check("H_six_transitions_primary_in_target_pair", ok, 1);
	}

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}
