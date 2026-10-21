/* gcc -Wall -Wextra -Werror -o /tmp/test_mlo_rx_freq_resolve test_mlo_rx_freq_resolve.c && /tmp/test_mlo_rx_freq_resolve
 *
 * Measured 2026-10-13 (0x5 -> 0x3, retired deflink L2 = wcid1 / band 1, incoming L1 = wcid3 / band 1, survivor L0 = wcid2):
 * every post-transition unicast frame received on 5240 MHz decrypts correctly but the RX descriptor's WLAN_IDX is the
 * retired wcid1 (link 2): 682/270/137/74/22 frames in five runs, 100 %. mac80211 then sees them on an inactive link.
 */
#include <stdio.h>
#include "mlo_rx_freq_resolve.h"

static int failures;
static void check(const char *n, long got, long want)
{
	if (got != want) { printf("FAIL %s: got=%ld want=%ld\n", n, got, want); failures++; }
	else printf("PASS %s\n", n);
}

int main(void)
{
	struct mt7925_mlo_rx_freq_cand c[3] = {
		{ .usable = 1, .freq = 2462 },	/* L0 survivor */
		{ .usable = 1, .freq = 5240 },	/* L1 incoming */
		{ .usable = 0, .freq = 6535 },	/* L2 retired (valid=0, not active) */
	};

	check("A_retired_wcid_remapped_to_incoming_by_freq", mt7925_mlo_rx_freq_select(c, 3, 5240, 2), 1);
	check("A2_legacy_keeps_retired", mt7925_mlo_legacy_rx_freq_select(2), 2);
	check("B_correct_attribution_kept", mt7925_mlo_rx_freq_select(c, 3, 5240, 1), 1);
	check("B2_survivor_kept", mt7925_mlo_rx_freq_select(c, 3, 2462, 0), 0);
	check("C_no_usable_link_on_that_freq", mt7925_mlo_rx_freq_select(c, 3, 6535, 2), -1);
	check("C2_unknown_freq", mt7925_mlo_rx_freq_select(c, 3, 5180, 2), -1);
	check("C3_zero_freq_never_remaps", mt7925_mlo_rx_freq_select(c, 3, 0, 2), -1);
	check("D_resolved_wrong_but_usable_is_remapped", mt7925_mlo_rx_freq_select(c, 3, 2462, 1), 0);
	c[0].freq = 5240;
	check("E_two_usable_links_on_one_freq_is_ambiguous", mt7925_mlo_rx_freq_select(c, 3, 5240, 2), -1);
	c[0].freq = 2462;
	/* topology independence: any (survivor, incoming, retired) permutation of three links */
	{
		int ok = 1; unsigned s, i, r;
		unsigned f[3] = { 2462, 5240, 6535 };
		for (s = 0; s < 3; s++) for (i = 0; i < 3; i++) for (r = 0; r < 3; r++) {
			struct mt7925_mlo_rx_freq_cand t[3];
			unsigned k;
			if (s == i || s == r || i == r) continue;
			for (k = 0; k < 3; k++) { t[k].freq = f[k]; t[k].usable = (k == s || k == i); }
			if (mt7925_mlo_rx_freq_select(t, 3, f[i], r) != (int)i) ok = 0;
			if (mt7925_mlo_rx_freq_select(t, 3, f[s], r) != (int)s) ok = 0;
		}
		check("F_all_six_permutations", ok, 1);
	}
	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL PASS\n"); return 0;
}
