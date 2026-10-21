/* SPDX-License-Identifier: ISC */
/* Standalone contract test; plain userspace C, no kernel headers.
 *
 * Make-before-break passivity: with no pair transition holding a
 * security-valid continuity grant, the grant-incarnation bookkeeping and the
 * continuity guard must not change ordinary initial-association behaviour
 * (initial JOIN, grants incl. SUB_LINK, retried JOIN tokens beyond the
 * table size, association-time aborts of the init tokens).
 */
#include <stdio.h>
#include "mlo_roc_state.h"
#include "mlo_pair_transition.h"

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

static int allowed(int v)
{
	return mt7925_mlo_roc_abort_verdict_allows(v);
}

int main(void)
{
	struct mt7925_mlo_roc_state st = {};
	const unsigned int bss_of_link[3] = { 2, 1, 0 };
	unsigned int tok, assoc, all_allowed = 1, owner_seen = 0;
	int v;

	/* association-time single-link JOIN tokens 1 and 2 (link2/bss0):
	 * aborted before and after the association completes
	 */
	for (tok = 1; tok <= 2; tok++)
		for (assoc = 0; assoc <= 1; assoc++) {
			v = mt7925_mlo_roc_abort_check(&st, tok, 0, assoc, 0);
			all_allowed &= allowed(v);
		}
	check("init_token_aborts_allowed", all_allowed, 1);

	/* initial 0x6 JOIN token 3: ACQUIRE link2/bss0 + SUB_LINK link1/bss1,
	 * both granted; the host records them as plain HELD incarnations
	 */
	st.held_token = 3;
	st.held_links = 0x6;
	st.link_token[1] = 3;
	st.link_token[2] = 3;
	mt7925_mlo_grant_note(&st, 3, 2, 0, 0xfe, 0, MT7925_MLO_GRANT_HELD);
	mt7925_mlo_grant_note(&st, 3, 1, 1, 0xfe, 0, MT7925_MLO_GRANT_HELD);
	check("no_owner_after_initial_grant", mt7925_mlo_continuity_get(&st) < 0, 1);

	/* initial grow marks with secure_rx=0 (no valid RX yet): no owner */
	v = mt7925_mlo_continuity_mark(&st, 3, 2, 0, 1, 53, 0);
	check("grow_mark_no_secure_rx_no_owner",
	      mt7925_mlo_continuity_get(&st) < 0, 1);
	check("grow_mark_keeps_plain_held", st.grant[v].life,
	      MT7925_MLO_GRANT_HELD);

	/* every abort of the initial slots is still allowed */
	check("abort_link2_allowed",
	      allowed(mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0)), 1);
	check("abort_link1_allowed",
	      allowed(mt7925_mlo_roc_abort_check(&st, 3, 1, 1, 0)), 1);
	check("guard_mask_passes_everything",
	      mt7925_mlo_roc_guard_mask(&st, 0x6, bss_of_link, 1, 0), 0x6);
	check("teardown_guard_mask_passes",
	      mt7925_mlo_roc_guard_mask(&st, 0x6, bss_of_link, 0, 0), 0x6);

	/* retried associations: tokens 4..19 recorded far beyond the table
	 * capacity; recording may fail silently but never creates an owner
	 * and never makes an abort refused
	 */
	for (tok = 4; tok <= 19; tok++) {
		mt7925_mlo_grant_note(&st, tok, 2, 0, 1, 53,
				      MT7925_MLO_GRANT_HELD);
		mt7925_mlo_grant_note(&st, tok, 1, 1, 1, 40,
				      MT7925_MLO_GRANT_HELD);
		if (mt7925_mlo_continuity_get(&st) >= 0)
			owner_seen = 1;
		for (assoc = 0; assoc <= 1; assoc++) {
			all_allowed &= allowed(mt7925_mlo_roc_abort_check(&st,
							tok, 0, assoc, 0));
			all_allowed &= allowed(mt7925_mlo_roc_abort_check(&st,
							tok, 1, assoc, 0));
		}
	}
	check("retries_never_create_owner", owner_seen, 0);
	check("retries_aborts_all_allowed", all_allowed, 1);
	check("full_table_guard_mask_passes",
	      mt7925_mlo_roc_guard_mask(&st, 0x6, bss_of_link, 1, 0), 0x6);

	/* late grant of a cancelled token is an orphan and may be released */
	mt7925_mlo_grant_note(&st, 20, 0xff, 1, 1, 40, MT7925_MLO_GRANT_ORPHAN);
	check("orphan_abort_allowed",
	      allowed(mt7925_mlo_roc_abort_check(&st, 20, 1, 1, 0)), 1);

	/* the guard never touches host held/pending bookkeeping */
	check("held_links_untouched", st.held_links, 0x6);
	check("held_token_untouched", st.held_token, 3);

	/* destructive gates are off at production defaults */
	check("destructive_gate_off_defaults",
	      mt7925_mlo_pt_destructive_gate(0, 0, 0, 0, 3), 0);

	/* release_all at association teardown clears every incarnation */
	{
		unsigned int t;

		mt7925_mlo_roc_state_release_all(&st, &t);
		check("release_all_clears_owner",
		      mt7925_mlo_continuity_get(&st) < 0, 1);
		check("release_all_clears_grants",
		      mt7925_mlo_grant_find(&st, 3, 0) < 0, 1);
	}

	if (failures) {
		printf("test_mlo_mbb_passive: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("test_mlo_mbb_passive: ALL PASS\n");
	return 0;
}
