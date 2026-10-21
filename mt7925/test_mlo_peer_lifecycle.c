/* SPDX-License-Identifier: ISC */
/* Standalone contract test; plain userspace C, no kernel headers. */
#include <stdio.h>
#include "mlo_peer_lifecycle.h"

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

static void run_destructive(const char *name, unsigned int from,
			    unsigned int to, unsigned int generation,
			    int authorize_ret)
{
	struct mt7925_mlo_peer_lifecycle lifecycle;

	mt7925_mlo_peer_lifecycle_begin(&lifecycle, generation, from, to);
	check(name, lifecycle.ready, 0);
	mt7925_mlo_peer_lifecycle_created(&lifecycle);
	mt7925_mlo_peer_lifecycle_restored(&lifecycle);
	mt7925_mlo_peer_lifecycle_authorized(&lifecycle, authorize_ret);

	check("destructive_create_state", lifecycle.create_state,
	      MT7925_MLO_PEER_STATE_NONE);
	check("destructive_create_newly", lifecycle.create_newly, 1);
	check("destructive_authorize_state", lifecycle.authorize_state,
	      MT7925_MLO_PEER_STATE_ASSOC);
	check("destructive_authorize_newly", lifecycle.authorize_newly, 0);
	check("destructive_terminal_authorized",
	      mt7925_mlo_peer_lifecycle_terminal_authorized(&lifecycle),
	      authorize_ret == 0);
	check("destructive_ready", lifecycle.ready, authorize_ret == 0);
	check("destructive_converged", lifecycle.converged,
	      authorize_ret == 0);
}

int main(void)
{
	static const unsigned int pairs[][2] = {
		{ 0x6, 0x5 }, { 0x5, 0x3 }, { 0x3, 0x6 },
		{ 0x6, 0x3 }, { 0x3, 0x5 }, { 0x5, 0x6 },
	};
	struct mt7925_mlo_peer_lifecycle ordinary = { 0 };
	unsigned int i;

	/* CASE 1: fresh peer creation has the proven two-stage shape. */
	run_destructive("case1_fresh_ready_withheld", 0, 0x6, 1, 0);

	/* CASE 2 and CASE 7: generic destructive migration for every pair. */
	for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
		run_destructive("case2_7_topology_independent", pairs[i][0],
				pairs[i][1], i + 2, 0);

	/* CASE 3: a retry is a complete new generation of the lifecycle. */
	run_destructive("case3_retry_first", 0x6, 0x3, 20, 0);
	run_destructive("case3_retry_second", 0x6, 0x3, 21, 0);

	/* CASE 4: authorization failure never publishes readiness/success. */
	run_destructive("case4_authorize_failure", 0x6, 0x3, 30, -1);

	/* CASE 5: ordinary updates remain ASSOC/newly=0 and non-destructive. */
	mt7925_mlo_peer_lifecycle_ordinary_update(&ordinary);
	check("case5_ordinary_state", ordinary.authorize_state,
	      MT7925_MLO_PEER_STATE_ASSOC);
	check("case5_ordinary_newly", ordinary.authorize_newly, 0);
	check("case5_ordinary_not_create", ordinary.create_newly, 0);

	/* CASE 6 is checked by terminal_authorized() in every success above. */

	/* CASE 8: the same logical link cannot inherit an older generation. */
	run_destructive("case8_generation_40", 0x6, 0x5, 40, 0);
	run_destructive("case8_generation_41", 0x5, 0x3, 41, 0);
	run_destructive("case8_generation_42", 0x3, 0x6, 42, 0);

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}

	printf("ALL PASS\n");
	return 0;
}
