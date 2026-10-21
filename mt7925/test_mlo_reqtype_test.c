/* SPDX-License-Identifier: ISC */
/* Standalone test for delta-only request-family policy; no kernel headers. */
#include <stdio.h>
#include "mlo_reqtype_test.h"

static int failures;

static void check(const char *name, int got, int want)
{
	if (got != want) {
		printf("FAIL %s: got=%d want=%d\n", name, got, want);
		failures++;
	}
}

int main(void)
{
	static const int types[] = { 0, 10, 11 };
	unsigned int i, j;

	check("auto_is_valid", mt7925_mlo_test_reqtype_valid(-1), 1);
	check("join_is_valid", mt7925_mlo_test_reqtype_valid(0), 1);
	check("ag_is_valid", mt7925_mlo_test_reqtype_valid(10), 1);
	check("aa_is_valid", mt7925_mlo_test_reqtype_valid(11), 1);
	check("minus_two_rejected", mt7925_mlo_test_reqtype_valid(-2), 0);
	check("roc_rejected", mt7925_mlo_test_reqtype_valid(1), 0);
	check("sub_link_rejected", mt7925_mlo_test_reqtype_valid(2), 0);
	check("sentinel_rejected", mt7925_mlo_test_reqtype_valid(12), 0);

	for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		check("auto_preserves_family",
		      mt7925_mlo_test_reqtype_resolve(-1, 1, types[i]), types[i]);
		check("invalid_preserves_family",
		      mt7925_mlo_test_reqtype_resolve(12, 1, types[i]), types[i]);
		for (j = 0; j < sizeof(types) / sizeof(types[0]); j++) {
			check("non_delta_preserves_family",
			      mt7925_mlo_test_reqtype_resolve(types[j], 0, types[i]),
			      types[i]);
			check("delta_override_changes_family",
			      mt7925_mlo_test_reqtype_resolve(types[j], 1, types[i]),
			      types[j]);
		}
	}
	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures != 0;
}
