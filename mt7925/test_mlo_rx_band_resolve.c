/* Standalone contract test for MLO RX band/WLAN_IDX cross-check. Build and
 * run:
 *   gcc -Wall -Wextra -Werror -o /tmp/test_mlo_rx_band_resolve \
 *       test_mlo_rx_band_resolve.c && /tmp/test_mlo_rx_band_resolve
 */
#include <stdio.h>
#include "mlo_rx_band_resolve.h"

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
	/* CASE A: no wcid resolved at all -- nothing to cross-check. */
	{
		struct mt7925_mlo_rx_band_ctx c = { .has_wcid = 0,
						     .wcid_phy_idx = 0,
						     .desc_band_idx = 1 };
		check("A_no_wcid_no_mismatch", mt7925_mlo_rx_band_mismatch(&c), 0);
	}

	/* CASE B: raw idx resolved to the same band the descriptor says --
	 * the common, correct case. */
	{
		struct mt7925_mlo_rx_band_ctx c = { .has_wcid = 1,
						     .wcid_phy_idx = 0,
						     .desc_band_idx = 0 };
		check("B_band_matches_no_mismatch", mt7925_mlo_rx_band_mismatch(&c), 0);
	}

	/* CASE C: raw idx resolved to a wcid on band 1, but the frame's
	 * descriptor says band 0 -- the live-proven collision. */
	{
		struct mt7925_mlo_rx_band_ctx c = { .has_wcid = 1,
						     .wcid_phy_idx = 1,
						     .desc_band_idx = 0 };
		check("C_band_mismatch_detected", mt7925_mlo_rx_band_mismatch(&c), 1);
	}

	/* CASE D: exactly one affiliated link owns the descriptor's band --
	 * unambiguous remap target. */
	{
		struct mt7925_mlo_rx_band_candidate cands[3] = {
			{ .present = 1, .phy_idx = 0 },
			{ .present = 1, .phy_idx = 1 },
			{ .present = 0, .phy_idx = 0 },
		};
		check("D_unique_candidate_selected",
		      mt7925_mlo_rx_band_select(cands, 3, 0), 0);
		check("D_unique_candidate_selected_other_band",
		      mt7925_mlo_rx_band_select(cands, 3, 1), 1);
	}

	/* CASE E: no candidate owns that band -- caller must not guess. */
	{
		struct mt7925_mlo_rx_band_candidate cands[2] = {
			{ .present = 1, .phy_idx = 0 },
			{ .present = 0, .phy_idx = 1 },
		};
		check("E_no_candidate_returns_negative",
		      mt7925_mlo_rx_band_select(cands, 2, 1), -1);
	}

	/* CASE F: two affiliated links somehow both claim the same band --
	 * ambiguous, must not guess (never expected for client-mode STA,
	 * but the selector must stay safe if the model is ever wrong). */
	{
		struct mt7925_mlo_rx_band_candidate cands[2] = {
			{ .present = 1, .phy_idx = 1 },
			{ .present = 1, .phy_idx = 1 },
		};
		check("F_ambiguous_duplicate_band_returns_negative",
		      mt7925_mlo_rx_band_select(cands, 2, 1), -1);
	}

	/* CASE G: not-present slots are skipped even if their phy_idx field
	 * happens to hold stale/zeroed data matching desc_band_idx. */
	{
		struct mt7925_mlo_rx_band_candidate cands[2] = {
			{ .present = 0, .phy_idx = 0 },
			{ .present = 1, .phy_idx = 0 },
		};
		check("G_not_present_slot_skipped",
		      mt7925_mlo_rx_band_select(cands, 2, 0), 1);
	}

	/* CASE G/H: the cross-check is only as good as the per-link phy_idx it is
	 * fed. Live (Oct 11): peers created at phy_idx 0 for an anchor on band 1
	 * leave two siblings claiming band 0, so a band-0 frame that the firmware
	 * resolved to the anchor's WCID is "not a mismatch" and a band-1 frame has
	 * no candidate. With phy_idx synced to the link's band both are fixed.
	 */
	{
		struct mt7925_mlo_rx_band_ctx anchor_gets_band0_frame = {
			.has_wcid = 1, .wcid_phy_idx = 0, .desc_band_idx = 0 };
		struct mt7925_mlo_rx_band_candidate stale[2] = {
			{ .present = 1, .phy_idx = 0 }, /* link0 (2.4G on band 0) */
			{ .present = 1, .phy_idx = 0 }, /* link1 anchor (5G, really band 1) */
		};
		struct mt7925_mlo_rx_band_candidate synced[2] = {
			{ .present = 1, .phy_idx = 0 },
			{ .present = 1, .phy_idx = 1 },
		};
		struct mt7925_mlo_rx_band_ctx synced_anchor_band0_frame = {
			.has_wcid = 1, .wcid_phy_idx = 1, .desc_band_idx = 0 };

		check("G_stale_phy_idx_band0_frame_on_anchor_not_detected",
		      mt7925_mlo_rx_band_mismatch(&anchor_gets_band0_frame), 0);
		check("G_stale_phy_idx_band0_ambiguous",
		      mt7925_mlo_rx_band_select(stale, 2, 0), -1);
		check("G_stale_phy_idx_band1_has_no_owner",
		      mt7925_mlo_rx_band_select(stale, 2, 1), -1);
		check("H_synced_phy_idx_band0_frame_on_anchor_detected",
		      mt7925_mlo_rx_band_mismatch(&synced_anchor_band0_frame), 1);
		check("H_synced_phy_idx_band0_goes_to_link0",
		      mt7925_mlo_rx_band_select(synced, 2, 0), 0);
		check("H_synced_phy_idx_band1_goes_to_link1",
		      mt7925_mlo_rx_band_select(synced, 2, 1), 1);
	}

	/* CASES I-K: after a late add (0x5 -> 0x1 -> 0x3) the firmware keeps naming the retired deflink (wcid1, link 2, band 1)
	 * for frames received on the incoming link (link 1, also band 1). The retired WCID's band equals the descriptor's band,
	 * so the stock check sees no mismatch and the frame stays on the retired link (measured 2026-10-13: 100 % of
	 * 682/270/137/74/22 frames).
	 */
	{
		struct mt7925_mlo_rx_band_ctx retired_resolved = {
			.has_wcid = 1, .wcid_phy_idx = 1, .desc_band_idx = 1, .wcid_retired = 1 };
		struct mt7925_mlo_rx_band_ctx live_resolved = {
			.has_wcid = 1, .wcid_phy_idx = 1, .desc_band_idx = 1, .wcid_retired = 0 };
		/* link ids 0..2: survivor L0 band 0, incoming L1 band 1, retired L2 band 1 (not usable -> not present) */
		struct mt7925_mlo_rx_band_candidate after_late_add[3] = {
			{ .present = 1, .phy_idx = 0 },
			{ .present = 1, .phy_idx = 1 },
			{ .present = 0, .phy_idx = 1 },
		};

		check("I_retired_wcid_with_matching_band_is_a_mismatch",
		      mt7925_mlo_rx_band_mismatch(&retired_resolved), 1);
		check("J_retired_link_excluded_so_band1_goes_to_incoming_link",
		      mt7925_mlo_rx_band_select(after_late_add, 3, 1), 1);
		check("K_live_wcid_with_matching_band_is_kept",
		      mt7925_mlo_rx_band_mismatch(&live_resolved), 0);
		check("K2_band0_frame_still_goes_to_survivor",
		      mt7925_mlo_rx_band_select(after_late_add, 3, 0), 0);
	}

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}
