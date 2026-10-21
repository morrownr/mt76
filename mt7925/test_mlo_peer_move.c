/* gcc -Wall -Wextra -Werror -o /tmp/test_mlo_peer_move test_mlo_peer_move.c && /tmp/test_mlo_peer_move */
#include <stdio.h>
#include "mlo_peer_move.h"

static int failures;
static void check(const char *n, int got, int want)
{
	if (got != want) { printf("FAIL %s: got=%d want=%d\n", n, got, want); failures++; }
	else printf("PASS %s\n", n);
}

int main(void)
{
	/* unchanged anchor on band 1 with the creation-time phy_idx 0: must NOT be rebuilt when the knob is on */
	check("A_unchanged_anchor_not_rebuilt", mt7925_mlo_peer_rebuild_needed(0, 1, false, true), 0);
	/* legacy behaviour (knob off) rebuilt it: documents the defect */
	check("A2_legacy_rebuilds_unchanged_anchor", mt7925_mlo_peer_rebuild_needed(0, 1, false, false), 1);
	/* a peer whose band really moved and whose bookkeeping differs is still rebuilt */
	check("B_moved_peer_rebuilt", mt7925_mlo_peer_rebuild_needed(0, 1, true, true), 1);
	/* bookkeeping already equals the new band: never rebuilt */
	check("C_phy_equals_band_not_rebuilt", mt7925_mlo_peer_rebuild_needed(1, 1, true, true), 0);
	check("C2_phy_equals_band_legacy", mt7925_mlo_peer_rebuild_needed(0, 0, false, false), 0);
	/* restore link_valid exactly when refresh will skip the peer */
	check("D_restore_when_refresh_skips", mt7925_mlo_peer_restore_valid_after_move(true, true, 0, 0), 1);
	check("D2_no_restore_when_refresh_rebuilds", mt7925_mlo_peer_restore_valid_after_move(true, true, 0, 1), 0);
	check("D3_no_restore_knob_off", mt7925_mlo_peer_restore_valid_after_move(false, true, 0, 0), 0);
	check("D4_no_restore_without_peer", mt7925_mlo_peer_restore_valid_after_move(true, false, 0, 0), 0);
	/* consistency: a peer is either restored or rebuilt-by-refresh, never both lost */
	{
		int ok = 1;
		unsigned int phy, band;
		for (phy = 0; phy < 2; phy++)
			for (band = 0; band < 2; band++) {
				int rebuild = mt7925_mlo_peer_rebuild_needed(phy, band, true, true);
				int restore = mt7925_mlo_peer_restore_valid_after_move(true, true, phy, band);
				if (rebuild == restore) /* exactly one of them must act */
					ok = 0;
			}
		check("E_exactly_one_of_rebuild_or_restore_acts", ok, 1);
	}
	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL PASS\n"); return 0;
}
