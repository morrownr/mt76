/* SPDX-License-Identifier: ISC */
/* Standalone contract test; plain userspace C, no kernel headers. */
#include <stdio.h>
#include "mlo_roc_delta.h"

static int failures;

static void check(const char *name, unsigned int got, unsigned int want)
{
	if (got != want) {
		printf("FAIL %s: got=0x%x want=0x%x\n", name, got, want);
		failures++;
	} else {
		printf("PASS %s\n", name);
	}
}

static void swap(const char *name, unsigned int old, unsigned int new,
		 unsigned int rem, unsigned int surv, unsigned int add)
{
	struct mt7925_mlo_roc_delta d;
	char n[96];

	mt7925_mlo_roc_delta(old, new, 0x7, &d);
	snprintf(n, sizeof(n), "%s_removed", name);
	check(n, d.removed, rem);
	snprintf(n, sizeof(n), "%s_survivor", name);
	check(n, d.survivor, surv);
	snprintf(n, sizeof(n), "%s_added", name);
	check(n, d.added, add);
	snprintf(n, sizeof(n), "%s_legal", name);
	check(n, d.legal_swap, 1);

	/* Old JOIN token holds both old links: only the removed slot goes. */
	snprintf(n, sizeof(n), "%s_release_only_removed", name);
	check(n, mt7925_mlo_roc_release_mask(old, new, 0x7, old), rem);
}

int main(void)
{
	struct mt7925_mlo_roc_delta d;

	swap("0x6_to_0x5", 0x6, 0x5, 0x2, 0x4, 0x1);
	swap("0x5_to_0x3", 0x5, 0x3, 0x4, 0x1, 0x2);
	swap("0x3_to_0x6", 0x3, 0x6, 0x1, 0x2, 0x4);
	swap("0x6_to_0x3", 0x6, 0x3, 0x4, 0x2, 0x1);
	swap("0x3_to_0x5", 0x3, 0x5, 0x2, 0x1, 0x4);
	swap("0x5_to_0x6", 0x5, 0x6, 0x1, 0x4, 0x2);

	/* mac80211 splits a swap into shrink-to-survivor then grow. */
	check("phase1_0x6_to_0x4_release", mt7925_mlo_roc_release_mask(0x6, 0x4, 0x7, 0x6), 0x2);
	check("phase1_0x5_to_0x1_release", mt7925_mlo_roc_release_mask(0x5, 0x1, 0x7, 0x5), 0x4);
	check("phase1_0x3_to_0x2_release", mt7925_mlo_roc_release_mask(0x3, 0x2, 0x7, 0x3), 0x1);

	/* old == new: nothing to release, not a swap. */
	mt7925_mlo_roc_delta(0x6, 0x6, 0x7, &d);
	check("same_not_legal", d.legal_swap, 0);
	check("same_release_none", mt7925_mlo_roc_release_mask(0x6, 0x6, 0x7, 0x6), 0);

	/* zero-sided (association setup/teardown): keep release-everything. */
	check("teardown_release_all", mt7925_mlo_roc_release_mask(0x6, 0x0, 0x7, 0x6), 0x6);
	check("setup_release_all", mt7925_mlo_roc_release_mask(0x0, 0x6, 0x7, 0x6), 0x6);
	mt7925_mlo_roc_delta(0x0, 0x6, 0x7, &d);
	check("setup_not_legal", d.legal_swap, 0);

	/* disjoint pair change is not a one-out/one-in swap. */
	mt7925_mlo_roc_delta(0x3, 0x4, 0x7, &d);
	check("shrink_not_legal", d.legal_swap, 0);

	/* malformed / outside valid_links: fall back to full release. */
	mt7925_mlo_roc_delta(0x6, 0x9, 0x7, &d);
	check("outside_valid_not_legal", d.legal_swap, 0);
	check("outside_valid_release_all", mt7925_mlo_roc_release_mask(0x6, 0x9, 0x7, 0x6), 0x6);
	mt7925_mlo_roc_delta(0x7, 0x3, 0x7, &d);
	check("three_active_not_legal", d.legal_swap, 0);

	/* Replacement JOIN: survivor slot preserved, other stale slots freed. */
	check("stale_keep_survivor", mt7925_mlo_roc_stale_release_mask(0x6, 0x5), 0x2);
	check("stale_same_pair_none", mt7925_mlo_roc_stale_release_mask(0x6, 0x6), 0);
	check("stale_disjoint_all", mt7925_mlo_roc_stale_release_mask(0x3, 0x4), 0x3);

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures != 0;
}
