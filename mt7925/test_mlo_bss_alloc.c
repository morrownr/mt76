/* gcc -Wall -Wextra -Werror -o /tmp/test_mlo_bss_alloc test_mlo_bss_alloc.c && /tmp/test_mlo_bss_alloc */
#include <stdio.h>
#include "mlo_bss_alloc.h"

static int failures;
static void check(const char *n, long got, long want)
{
	if (got != want) { printf("FAIL %s: got=%ld want=%ld\n", n, got, want); failures++; }
	else printf("PASS %s\n", n);
}

int main(void)
{
	/* 0x5 -> 0x3 (association order: L2 bss0, L0 bss1, L1 bss2): L1 is dormant and re-added while L2 is retired */
	unsigned long long mask = 0x3; /* bss0 (retired L2) + bss1 (survivor L0) in use; dormant L1 holds none */

	check("A_stock_incoming_gets_bss2_not_attachable", mt7925_mlo_bss_idx_attachable(mt7925_mlo_lowest_free_bss(mask)), 0);
	mask = mt7925_mlo_vif_mask_after_retire(mask, 0);
	check("B_after_retire_incoming_gets_bss0", mt7925_mlo_lowest_free_bss(mask), 0);
	check("C_after_retire_attachable", mt7925_mlo_bss_idx_attachable(mt7925_mlo_lowest_free_bss(mask)), 1);
	/* survivor's bit is never cleared by retiring another link */
	check("D_survivor_bit_kept", (int)((mask >> 1) & 1), 1);
	/* adding the incoming link then fills bss0; a later re-add of the retired link would get bss2 (not attachable by
	 * itself) -- documented limitation, the next retire frees a low index again
	 */
	mask |= 1ULL << mt7925_mlo_lowest_free_bss(mask);
	check("E_next_free_is_bss2", mt7925_mlo_lowest_free_bss(mask), 2);
	check("F_bss2_not_attachable", mt7925_mlo_bss_idx_attachable(2), 0);
	check("G_empty_mask_gives_zero", mt7925_mlo_lowest_free_bss(0), 0);
	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL PASS\n");
	return 0;
}
