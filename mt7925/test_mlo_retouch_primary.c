/* gcc -Wall -Wextra -Werror -o /tmp/test_mlo_retouch_primary test_mlo_retouch_primary.c && /tmp/test_mlo_retouch_primary
 *
 * The primary "retouch" that precedes an incoming partner's STA_REC (and carries the two-link STA_REC_MLD) must be
 * addressed to a LIVE MLD member. Measured 2026-10-13 (0x5 -> 0x3, survivor L0 wcid2, retired host deflink L2 wcid1):
 * the retouch went to retired wcid1 and the survivor never received the update, so firmware kept wcid2 related to wcid1
 * and left incoming wcid3 unattached.
 */
#include <stdio.h>
#include "mlo_retouch_primary.h"

#define B(n) (1UL << (n))
static int failures;
static void check(const char *n, long got, long want)
{
	if (got != want) { printf("FAIL %s: got=%ld want=%ld\n", n, got, want); failures++; }
	else printf("PASS %s\n", n);
}

int main(void)
{
	/* 0x5 -> 0x3: deflink L2 retired (not valid), survivor L0 active+valid, incoming L1 */
	check("A_retired_deflink_not_subject", mt7925_mlo_retouch_primary_link(2, B(0) | B(1), B(0), 1), 0);
	/* legacy behaviour addressed the deflink: documents the defect */
	check("A2_legacy_subject_was_deflink", mt7925_mlo_legacy_retouch_primary_link(2), 2);
	/* live deflink stays the subject (initial association and 0x6->0x3 with deflink surviving) */
	check("B_live_deflink_kept", mt7925_mlo_retouch_primary_link(0, B(0) | B(1), B(0), 1), 0);
	check("B2_live_deflink_l2_kept", mt7925_mlo_retouch_primary_link(2, B(2) | B(1), B(2), 1), 2);
	/* incoming link is never its own retouch subject, even if mac80211 already lists it active and it looks valid */
	check("C_incoming_excluded", mt7925_mlo_retouch_primary_link(1, B(0) | B(1), B(0) | B(1), 1), 0);
	/* no live member: caller keeps legacy subject (initial association has no published active links yet) */
	check("D_none_live", mt7925_mlo_retouch_primary_link(0, 0, 0, 1), -1);
	check("D2_retired_only", mt7925_mlo_retouch_primary_link(2, B(1), B(2), 1), -1);
	/* topology independence: every (deflink, survivor, incoming) permutation of 3 links with a retired deflink */
	{
		int ok = 1;
		unsigned d, s, i;
		for (d = 0; d < 3; d++)
			for (s = 0; s < 3; s++)
				for (i = 0; i < 3; i++) {
					if (s == d || s == i || i == d)
						continue;
					if (mt7925_mlo_retouch_primary_link(d, B(s) | B(i), B(s), i) != (int)s)
						ok = 0;
				}
		check("E_all_six_retired_deflink_cases_pick_survivor", ok, 1);
	}
	/* never returns a link that is not both active and valid */
	{
		int ok = 1;
		unsigned long act, val;
		unsigned d, i;
		for (act = 0; act < 8; act++)
			for (val = 0; val < 8; val++)
				for (d = 0; d < 3; d++)
					for (i = 0; i < 3; i++) {
						int r = mt7925_mlo_retouch_primary_link(d, act, val, i);
						if (r >= 0 && (!(act & B(r)) || !(val & B(r)) || (unsigned)r == i))
							ok = 0;
					}
		check("F_result_always_active_valid_not_incoming", ok, 1);
	}
	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL PASS\n");
	return 0;
}
