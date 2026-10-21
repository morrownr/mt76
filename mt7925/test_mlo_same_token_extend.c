/* SPDX-License-Identifier: ISC */
/* Standalone contract test; plain userspace C, no kernel headers.
 *
 * Same-token two-TLV JOIN extension (mlo_same_token_extend, diagnostic
 * gate, default off): reuse the continuity owner's HELD token in a 2-TLV
 * JOIN (TLV0 re-affirms the already-granted survivor bss, TLV1 requests
 * the replacement bss), with no ROC_ABORT of the survivor ever sent. This
 * exercises the shared-token safety property the experiment depends on:
 * mt7925_mlo_roc_abort_check() must key strictly on (token, bss_idx), so
 * the survivor's grant stays BLOCK_CONTINUITY while the replacement's own
 * (token, bss_idx) -- same token, different bss -- stays freely abortable,
 * and the new eligibility helper must only fire when a continuity owner is
 * actually held and the requested pair is a legitimate one-link addition
 * to it.
 */
#include <stdio.h>
#include "mlo_roc_state.h"

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
	int i;

	/* --- TEST B: eligibility helper (new pure logic) --- */

	/* no held token at all: never eligible */
	check("not_eligible_no_held_token",
	      mt7925_mlo_same_token_extend_eligible(&st, 0x5), 0);

	/* survivor link2/bss0 held under token 3 (as in the live run) */
	st.held_token = 3;
	st.held_links = 0x4;
	st.link_token[2] = 3;

	/* requesting the survivor's own pair (0x5 = link0|link2, one link
	 * added beyond what's held) is eligible, and returns the held token
	 */
	check("eligible_one_link_add",
	      mt7925_mlo_same_token_extend_eligible(&st, 0x5), 3);

	/* requesting a pair that would need to DROP the held link is not a
	 * same-token extension (nothing to re-affirm as TLV0)
	 */
	check("not_eligible_drops_held_link",
	      mt7925_mlo_same_token_extend_eligible(&st, 0x3), 0);

	/* requesting two links beyond what's held is not a single extension */
	st.held_links = 0x0;
	st.held_token = 3;
	check("not_eligible_two_links_added",
	      mt7925_mlo_same_token_extend_eligible(&st, 0x5), 0);

	/* --- TEST A/D/E/F/G: shared-token abort safety --- */

	st = (struct mt7925_mlo_roc_state){};
	st.held_token = 3;
	st.held_links = 0x4;
	st.link_token[2] = 3;
	i = mt7925_mlo_grant_note(&st, 3, 2, 0, 1, 53, MT7925_MLO_GRANT_HELD);
	check("survivor_grant_recorded", i >= 0, 1);
	mt7925_mlo_continuity_mark(&st, 3, 2, 0, 1, 53, 1);

	/* TEST A/G: continuity owner (token=3, bss=0) stays BLOCK_CONTINUITY
	 * for the entire experiment -- gate on or off makes no difference to
	 * this property, since abort_check never consults the gate flag
	 * unless test_gate is explicitly passed.
	 */
	check("survivor_blocked_before_extend",
	      allowed(mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0)), 0);

	/* TEST C/D: extension pending for replacement link0/bss2, SAME token
	 * 3. The replacement's own (token=3, bss=2) is untracked (no grant
	 * recorded yet) and therefore freely abortable -- this is what lets
	 * a timed-out extension cancel only its own slot.
	 */
	mt7925_mlo_roc_state_attach_request(&st, 3, 0x1 /* link0 */,
					    1u << 2 /* bss2 */);
	check("replacement_slot_abortable_same_token",
	      allowed(mt7925_mlo_roc_abort_check(&st, 3, 2, 1, 0)), 1);

	/* TEST A/G: survivor still blocked while the extension is pending */
	check("survivor_still_blocked_during_extend",
	      allowed(mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0)), 0);

	/* TEST C: timeout cancels ONLY the replacement bss under the SAME
	 * token, held_links (survivor) is untouched.
	 */
	{
		unsigned int tok, links;

		links = mt7925_mlo_roc_state_attach_timeout(&st, &tok);
		check("extend_timeout_token_is_held_token", tok, 3);
		check("extend_timeout_cancels_only_replacement", links, 0x1);
		check("survivor_still_held_after_extend_timeout",
		      st.held_links, 0x4);
	}
	check("survivor_still_continuity_after_timeout",
	      st.grant[mt7925_mlo_continuity_get(&st)].continuity_owner, 1);

	/* TEST D: if instead the replacement bss GRANTS (same token), the
	 * survivor's own recorded grant entry must not be touched.
	 */
	st = (struct mt7925_mlo_roc_state){};
	st.held_token = 3;
	st.held_links = 0x4;
	st.link_token[2] = 3;
	i = mt7925_mlo_grant_note(&st, 3, 2, 0, 1, 53, MT7925_MLO_GRANT_HELD);
	mt7925_mlo_continuity_mark(&st, 3, 2, 0, 1, 53, 1);
	mt7925_mlo_roc_state_attach_request(&st, 3, 0x1, 1u << 2);
	check("attach_grant_matches_replacement_bss_only",
	      mt7925_mlo_roc_state_attach_grant(&st, 3, 2), 1);
	check("survivor_grant_unaffected_by_replacement_grant",
	      st.grant[mt7925_mlo_continuity_get(&st)].secure_rx, 1);
	check("held_links_now_includes_replacement",
	      st.held_links, 0x5);

	/* TEST E: a grant event for the SURVIVOR's own bss under the shared
	 * token (TLV0's echo) must NOT match the attach/extension waiter
	 * (attach_pending_bss names only the replacement bss).
	 */
	st = (struct mt7925_mlo_roc_state){};
	st.held_token = 3;
	st.held_links = 0x4;
	mt7925_mlo_roc_state_attach_request(&st, 3, 0x1, 1u << 2);
	check("survivor_echo_grant_does_not_match_extend_waiter",
	      mt7925_mlo_roc_state_attach_grant(&st, 3, 0 /* survivor bss */),
	      0);
	check("extend_still_pending_after_survivor_echo",
	      st.attach_pending_bss, 1u << 2);

	if (failures) {
		printf("FAILED: %d\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}
