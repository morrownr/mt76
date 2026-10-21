/* gcc -Wall -Wextra -Werror -o /tmp/test_mlo_wcid_handover test_mlo_wcid_handover.c && /tmp/test_mlo_wcid_handover
 *
 * The firmware keeps the association's first WCID (the deflink, wcid1) as the MLD's TX packet-number/key holder: while it
 * stays in firmware its PN counter keeps rising, frames addressed to any other WCID carry that WCID's own (low) PN and
 * the AP silently drops them (measured 2026-10-13: 0 of ~330 pings reached the AP's MLD netdev). Addressing the retired
 * deflink keeps PN continuity but its own link is dead (UL latency >= 100 ms). The incoming link on the same firmware
 * band slot as the retired deflink can inherit that WCID instead of getting a new one.
 */
#include <stdio.h>
#include "mlo_wcid_handover.h"

static int failures;
static void check(const char *n, long got, long want)
{
	if (got != want) { printf("FAIL %s: got=%ld want=%ld\n", n, got, want); failures++; }
	else printf("PASS %s\n", n);
}

#define B(n) (1UL << (n))

int main(void)
{
	/* 0x5 -> 0x1 -> 0x3: deflink L2 (band 1, retired), survivor L0 (band 0), incoming L1 (band 1) */
	unsigned int band[3] = { 0, 1, 1 };

	check("A_retired_deflink_same_band_is_donor",
	      mt7925_mlo_handover_donor(2, B(0) | B(2), B(0), band, 3, 1, 1), 2);
	check("A2_stock_never_hands_over", mt7925_mlo_legacy_handover_donor(), -1);
	check("B_different_band_no_donor",
	      mt7925_mlo_handover_donor(2, B(0) | B(2), B(0), band, 3, 0 + 1, 0), -1);
	check("C_live_deflink_never_donor",
	      mt7925_mlo_handover_donor(2, B(0) | B(2), B(0) | B(2), band, 3, 1, 1), -1);
	check("D_deflink_not_resident_no_donor",
	      mt7925_mlo_handover_donor(2, B(0), B(0), band, 3, 1, 1), -1);
	check("E_non_deflink_retired_link_is_not_donor",
	      mt7925_mlo_handover_donor(0, B(0) | B(2), 0UL, band, 3, 1, 1), -1);
	check("F_incoming_already_resident_no_handover",
	      mt7925_mlo_handover_donor(2, B(0) | B(1) | B(2), B(0), band, 3, 1, 1), -1);
	check("G_incoming_equals_deflink_no_handover",
	      mt7925_mlo_handover_donor(2, B(0) | B(2), B(0), band, 3, 2, 1), -1);
	/* 0x3 -> 0x1 -> 0x6 returns: deflink now L1 (band 1) retired, incoming L2 (band 1) */
	check("H_hand_back_to_L2",
	      mt7925_mlo_handover_donor(1, B(0) | B(1), B(0), band, 3, 2, 1), 1);
	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL PASS\n"); return 0;
}
