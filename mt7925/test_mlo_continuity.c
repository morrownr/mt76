/* SPDX-License-Identifier: ISC */
/* Standalone contract test; plain userspace C, no kernel headers.
 *
 * Make-before-break continuity: the security-valid grant carrying the MLD
 * (continuity grant) is never aborted before another security-valid path
 * exists. Scenarios A-F of the make-before-break task.
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

/* 0x6 -> 0x5 after the shrink: survivor link2 (bss0) held under token 3 and
 * carrying security-valid RX; link1 (bss1) already released as removed.
 */
static void setup_survivor(struct mt7925_mlo_roc_state *st)
{
	*st = (struct mt7925_mlo_roc_state){};
	st->held_token = 3;
	st->held_links = 0x4;
	st->link_token[2] = 3;
	mt7925_mlo_continuity_mark(st, 3, 2, 0, 1, 37, 1);
}

int main(void)
{
	struct mt7925_mlo_roc_state st;
	const unsigned int bss_of_link[3] = { 2, 1, 0 };
	int v, idx;

	/* ---- grant-incarnation bookkeeping ---- */
	setup_survivor(&st);
	idx = mt7925_mlo_continuity_get(&st);
	check("cont_exists", idx >= 0, 1);
	check("cont_token", st.grant[idx].token, 3);
	check("cont_bss", st.grant[idx].bss_idx, 0);
	check("cont_link", st.grant[idx].link_id, 2);
	check("cont_life", st.grant[idx].life, MT7925_MLO_GRANT_HELD_CONTINUITY);
	check("cont_owner", st.grant[idx].continuity_owner, 1);
	check("cont_secure", st.grant[idx].secure_rx, 1);
	/* keyed by (token, bss_idx), not link: another token on the same bss
	 * is a different incarnation
	 */
	check("key_token_bss", mt7925_mlo_grant_find(&st, 4, 0) < 0, 1);
	check("key_same", mt7925_mlo_grant_find(&st, 3, 0), (unsigned int)idx);

	/* ---- A: replacement blocked / times out ---- */
	setup_survivor(&st);
	/* the delta request (token 4, link0 bss2) times out: its own slot may
	 * be cancelled, the survivor's continuity slot may not
	 */
	mt7925_mlo_grant_note(&st, 4, 0, 2, 0xfe, 1, MT7925_MLO_GRANT_PREPARING);
	v = mt7925_mlo_roc_abort_check(&st, 4, 2, 1, 0);
	check("A_request_slot_abort_allowed", allowed(v), 1);
	v = mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0);
	check("A_continuity_abort_blocked", v, MT7925_MLO_ABORT_BLOCK_CONTINUITY);
	check("A_prepare_fail_state",
	      mt7925_mlo_pt_prepare_result(-110, 1), MT7925_MLO_PT_FAILED_SAFE);
	check("A_prepare_fail_reason",
	      mt7925_mlo_pt_prepare_reason(-110, 1),
	      MT7925_MLO_PT_REASON_PREPARE_BLOCKED_BY_HELD_GRANT);
	check("A_no_release_all_fallback",
	      mt7925_mlo_pt_destructive_fallback_allowed(
		      mt7925_mlo_pt_destructive_gate(0, 0, 0, 0, 3)), 0);
	/* the production transition graph cannot leave PREPARE_SHADOW_GROUP
	 * toward a survivor release
	 */
	check("A_shadow_to_release_survivor_illegal",
	      mt7925_mlo_pt_can_advance(MT7925_MLO_PT_PREPARE_SHADOW_GROUP,
					MT7925_MLO_PT_RELEASE_SURVIVOR_OWNERSHIP), 0);
	check("A_shadow_to_failsafe",
	      mt7925_mlo_pt_can_advance(MT7925_MLO_PT_PREPARE_SHADOW_GROUP,
					MT7925_MLO_PT_FAILED_SAFE), 1);
	check("A_continuity_still_held", st.held_links & 0x4, 0x4);

	/* ---- B: replacement grant, no secure RX yet ---- */
	setup_survivor(&st);
	mt7925_mlo_grant_note(&st, 4, 0, 2, 0, 1, MT7925_MLO_GRANT_HELD);
	mt7925_mlo_grant_validate(&st, 4, 2, 0);
	check("B_handoff_not_allowed", mt7925_mlo_continuity_handoff_allowed(&st), 0);
	v = mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0);
	check("B_continuity_abort_blocked", v, MT7925_MLO_ABORT_BLOCK_CONTINUITY);
	check("B_handoff_refused", mt7925_mlo_continuity_handoff(&st, 4, 2, 1) < 0, 1);
	check("B_owner_unchanged",
	      st.grant[mt7925_mlo_continuity_get(&st)].token, 3);
	check("B_validate_to_handoff_illegal",
	      mt7925_mlo_pt_can_advance(MT7925_MLO_PT_WAIT_REPLACEMENT_RX,
					MT7925_MLO_PT_HANDOFF_CONTINUITY), 0);

	/* ---- C: replacement proves secure RX ---- */
	setup_survivor(&st);
	mt7925_mlo_grant_note(&st, 4, 0, 2, 0, 1, MT7925_MLO_GRANT_HELD);
	/* before validation the old grant is not releasable */
	check("C_old_blocked_before_validate",
	      mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0),
	      MT7925_MLO_ABORT_BLOCK_CONTINUITY);
	mt7925_mlo_grant_validate(&st, 4, 2, 1);
	idx = mt7925_mlo_grant_find(&st, 4, 2);
	check("C_validated_life", st.grant[idx].life, MT7925_MLO_GRANT_VALIDATED);
	check("C_handoff_allowed", mt7925_mlo_continuity_handoff_allowed(&st), 1);
	v = mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0);
	check("C_old_abort_allowed_after_validate", allowed(v), 1);
	check("C_old_abort_reason", v, MT7925_MLO_ABORT_ALLOW_HANDED_OFF);
	/* shadow group replaces the survivor incarnation: old one obsolete */
	check("C_handoff_ok", mt7925_mlo_continuity_handoff(&st, 4, 2, 0) >= 0, 1);
	check("C_new_owner", st.grant[mt7925_mlo_continuity_get(&st)].token, 4);
	idx = mt7925_mlo_grant_find(&st, 3, 0);
	check("C_old_obsolete", st.grant[idx].life, MT7925_MLO_GRANT_OBSOLETE);
	check("C_old_not_owner", st.grant[idx].continuity_owner, 0);
	v = mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0);
	check("C_obsolete_abort_allowed", v, MT7925_MLO_ABORT_ALLOW_OBSOLETE);
	/* delta shape: survivor stays part of the final pair -> keep it held */
	setup_survivor(&st);
	mt7925_mlo_grant_note(&st, 4, 0, 2, 0, 1, MT7925_MLO_GRANT_HELD);
	mt7925_mlo_grant_validate(&st, 4, 2, 1);
	check("C_keep_old_handoff_ok",
	      mt7925_mlo_continuity_handoff(&st, 4, 2, 1) >= 0, 1);
	idx = mt7925_mlo_grant_find(&st, 3, 0);
	check("C_keep_old_still_held", st.grant[idx].life, MT7925_MLO_GRANT_HELD);
	/* graph: WAIT_RX -> VALIDATE -> HANDOFF -> MIGRATE -> RELEASE_OLD ->
	 * CONVERGED
	 */
	check("C_g1", mt7925_mlo_pt_can_advance(MT7925_MLO_PT_WAIT_REPLACEMENT_RX,
						MT7925_MLO_PT_VALIDATE_SHADOW_PATH), 1);
	check("C_g2", mt7925_mlo_pt_can_advance(MT7925_MLO_PT_VALIDATE_SHADOW_PATH,
						MT7925_MLO_PT_HANDOFF_CONTINUITY), 1);
	check("C_g3", mt7925_mlo_pt_can_advance(MT7925_MLO_PT_HANDOFF_CONTINUITY,
						MT7925_MLO_PT_MIGRATE_TRAFFIC), 1);
	check("C_g4", mt7925_mlo_pt_can_advance(MT7925_MLO_PT_MIGRATE_TRAFFIC,
						MT7925_MLO_PT_RELEASE_OLD_GRANT), 1);
	check("C_g5", mt7925_mlo_pt_can_advance(MT7925_MLO_PT_RELEASE_OLD_GRANT,
						MT7925_MLO_PT_CONVERGED), 1);
	check("C_no_skip_validate",
	      mt7925_mlo_pt_can_advance(MT7925_MLO_PT_VALIDATE_SHADOW_PATH,
					MT7925_MLO_PT_RELEASE_OLD_GRANT), 0);

	/* ---- D: late / orphan grant ---- */
	setup_survivor(&st);
	st.timedout_token = 4;
	/* a queued JOIN that named the survivor's bss0 arrives late */
	check("D_late", mt7925_mlo_roc_state_late_grant(&st, 4, 0), 1);
	mt7925_mlo_grant_note(&st, 4, 2, 0, 1, 37, MT7925_MLO_GRANT_ORPHAN);
	v = mt7925_mlo_roc_abort_check(&st, 4, 0, 1, 0);
	check("D_orphan_release_allowed", v, MT7925_MLO_ABORT_ALLOW_ORPHAN);
	v = mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0);
	check("D_continuity_untouched", v, MT7925_MLO_ABORT_BLOCK_CONTINUITY);
	idx = mt7925_mlo_continuity_get(&st);
	check("D_owner_still_token3", st.grant[idx].token, 3);

	/* ---- E: generic cleanup during the transition ---- */
	setup_survivor(&st);
	st.held_links = 0x6;
	st.link_token[1] = 3;
	check("E_release_all_mask_filtered",
	      mt7925_mlo_roc_guard_mask(&st, 0x6, bss_of_link, 1, 0), 0x2);
	check("E_selective_mask_kept", mt7925_mlo_roc_guard_mask(&st, 0x2,
							      bss_of_link, 1, 0), 0x2);
	check("E_survivor_only_mask_blocked",
	      mt7925_mlo_roc_guard_mask(&st, 0x4, bss_of_link, 1, 0), 0);
	/* association teardown releases everything */
	check("E_teardown_releases_all",
	      mt7925_mlo_roc_guard_mask(&st, 0x6, bss_of_link, 0, 0), 0x6);
	check("E_teardown_verdict", mt7925_mlo_roc_abort_check(&st, 3, 0, 0, 0),
	      MT7925_MLO_ABORT_ALLOW_TEARDOWN);
	/* release_all clears continuity bookkeeping */
	{
		unsigned int tok;

		mt7925_mlo_roc_state_release_all(&st, &tok);
		check("E_release_all_clears_cont",
		      mt7925_mlo_continuity_get(&st) < 0, 1);
	}
	/* a non-secure survivor is not a continuity owner */
	st = (struct mt7925_mlo_roc_state){};
	mt7925_mlo_continuity_mark(&st, 3, 2, 0, 1, 37, 0);
	check("E_insecure_not_protected",
	      allowed(mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0)), 1);

	/* ---- F: destructive test gates ---- */
	check("F_default_gate_off", mt7925_mlo_pt_destructive_gate(0, 0, 0, 0, 3), 0);
	check("F_roc_release_bit2_only_off",
	      mt7925_mlo_pt_destructive_gate(0, 0, 0, 0, 2), 0);
	check("F_sep18_gate_on", mt7925_mlo_pt_destructive_gate(1, 0, 0, 0, 3), 1);
	check("F_rejoin_gate_on", mt7925_mlo_pt_destructive_gate(0, 0, 0, 1, 3), 1);
	check("F_rebind_gate_on", mt7925_mlo_pt_destructive_gate(0, 1, 0, 0, 3), 1);
	check("F_parent_gate_on", mt7925_mlo_pt_destructive_gate(0, 0, 1, 0, 3), 1);
	check("F_release_all_bit8_on",
	      mt7925_mlo_pt_destructive_gate(0, 0, 0, 0, 10), 1);
	setup_survivor(&st);
	check("F_gate_allows_test_abort", mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 1),
	      MT7925_MLO_ABORT_ALLOW_TEST_GATE);
	check("F_no_gate_blocks", mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0),
	      MT7925_MLO_ABORT_BLOCK_CONTINUITY);
	check("F_fallback_only_with_gate",
	      mt7925_mlo_pt_destructive_fallback_allowed(1), 1);
	/* production survivor handoff (release survivor) only with its gate */
	check("F_handoff_gate_off",
	      mt7925_mlo_pt_survivor_handoff_wanted(MT7925_MLO_PT_DELTA_ACQUIRE,
						    0, 0x4), 0);

	/* shrink: continuity is marked before the removed slot is released */
	check("shrink_order",
	      mt7925_mlo_pt_can_advance(MT7925_MLO_PT_PREPARE,
					MT7925_MLO_PT_PRESERVE_CONTINUITY_GRANT), 1);
	check("shrink_order2",
	      mt7925_mlo_pt_can_advance(MT7925_MLO_PT_PRESERVE_CONTINUITY_GRANT,
					MT7925_MLO_PT_QUIESCE_REMOVED), 1);
	check("grow_shadow",
	      mt7925_mlo_pt_can_advance(MT7925_MLO_PT_PRESERVE_SURVIVOR,
					MT7925_MLO_PT_PREPARE_SHADOW_GROUP), 1);
	check("shadow_admit",
	      mt7925_mlo_pt_can_advance(MT7925_MLO_PT_PREPARE_SHADOW_GROUP,
					MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER), 1);
	check("secure_rx_fresh", mt7925_mlo_pt_secure_rx_fresh(1, 200), 1);
	check("secure_rx_stale", mt7925_mlo_pt_secure_rx_fresh(1, 5000), 0);
	check("secure_rx_none", mt7925_mlo_pt_secure_rx_fresh(0, 0), 0);
	check("state_names", mt7925_mlo_pt_state_name(
		      MT7925_MLO_PT_RELEASE_OLD_GRANT)[0] == 'R', 1);

	/* ---- FAILED_SAFE active-mask publication (0x6 -> 0x5) ----
	 * mac80211 splits it into shrink 0x6->0x4 then grow 0x4->0x5; during
	 * the grow change_vif_links callback vif->active_links is transiently
	 * 0x5. The ungranted added link0 is never the published state: after
	 * the delta ACQUIRE times out mac80211 restores 0x4 (survivor-only).
	 * 0x6 is not restorable: link1's slot was released at the shrink.
	 */
	check("mask_prepare_shadow_not_published",
	      mt7925_mlo_pt_published_active(0x5, 0x1, 0x0), 0x4);
	check("mask_failsafe_timeout_published",
	      mt7925_mlo_pt_published_active(0x5, 0x1, 0x0), 0x4);
	check("mask_failsafe_never_link0",
	      mt7925_mlo_pt_published_active(0x5, 0x1, 0x0) & 0x1, 0);
	check("mask_failsafe_keeps_survivor",
	      mt7925_mlo_pt_published_active(0x5, 0x1, 0x0) & 0x4, 0x4);
	check("mask_granted_published",
	      mt7925_mlo_pt_published_active(0x5, 0x1, 0x1), 0x5);
	check("mask_no_add_passthrough",
	      mt7925_mlo_pt_published_active(0x6, 0x0, 0x0), 0x6);
	/* target is recorded separately and untouched by the failure */
	setup_survivor(&st);
	check("mask_failsafe_continuity_held",
	      mt7925_mlo_roc_abort_check(&st, 3, 0, 1, 0),
	      MT7925_MLO_ABORT_BLOCK_CONTINUITY);
	check("mask_failsafe_no_fallback",
	      mt7925_mlo_pt_destructive_fallback_allowed(
		      mt7925_mlo_pt_destructive_gate(0, 0, 0, 0, 3)), 0);

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures ? 1 : 0;
}
