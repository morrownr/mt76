/* SPDX-License-Identifier: ISC */
/* Standalone contract test; plain userspace C, no kernel headers. */
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

/* link0=2.4GHz, link1=5GHz, link2=6GHz */
static const int bands[3] = {
	MT7925_MLO_BAND_2G, MT7925_MLO_BAND_5G, MT7925_MLO_BAND_6G
};

static struct mt7925_mlo_pt_plan plan(unsigned int old_l, unsigned int new_l,
				      unsigned int held)
{
	struct mt7925_mlo_pt_plan p;

	mt7925_mlo_pt_plan(old_l, new_l, 0x7, held, bands, &p);
	return p;
}

int main(void)
{
	struct mt7925_mlo_pt_plan p;
	struct mt7925_mlo_roc_state st = {};
	unsigned int tok, mask;

	/* physical slot helper: 2.4 -> slot0, 5 and 6 share slot1 */
	check("slot_2g", mt7925_mlo_band_slot(MT7925_MLO_BAND_2G), 0);
	check("slot_5g", mt7925_mlo_band_slot(MT7925_MLO_BAND_5G), 1);
	check("slot_6g", mt7925_mlo_band_slot(MT7925_MLO_BAND_6G), 1);

	/* 0x6->0x5, after shrink: active 0x4, link2 slot held under old token */
	p = plan(0x4, 0x5, 0x4);
	check("0x4to0x5_kind", p.kind, MT7925_MLO_PT_DELTA_ACQUIRE);
	check("0x4to0x5_added", p.added, 0x1);
	check("0x4to0x5_survivor", p.survivor, 0x4);
	check("0x4to0x5_added_link", p.added_link, 0);
	check("0x4to0x5_added_slot", p.added_slot, 0);
	check("0x4to0x5_survivor_slot", p.survivor_slot, 1);

	/* 0x6->0x3, after shrink: survivor link1 (5G) held, add link0 */
	p = plan(0x2, 0x3, 0x2);
	check("0x2to0x3_kind", p.kind, MT7925_MLO_PT_DELTA_ACQUIRE);
	check("0x2to0x3_added_slot", p.added_slot, 0);

	/* 0x5->0x3, after shrink: survivor link0 (2.4) held, add link1 (5G) */
	p = plan(0x1, 0x3, 0x1);
	check("0x1to0x3_kind", p.kind, MT7925_MLO_PT_DELTA_ACQUIRE);
	check("0x1to0x3_added_slot", p.added_slot, 1);
	check("0x1to0x3_survivor_slot", p.survivor_slot, 0);

	/* 0x3->0x5, after shrink: survivor link0 held, add link2 (6G) */
	p = plan(0x1, 0x5, 0x1);
	check("0x1to0x5_kind", p.kind, MT7925_MLO_PT_DELTA_ACQUIRE);
	check("0x1to0x5_added_slot", p.added_slot, 1);

	/* into 0x6: added high-band link shares the survivor's slot1 */
	p = plan(0x4, 0x6, 0x4);
	check("0x4to0x6_kind", p.kind, MT7925_MLO_PT_HIGHBAND_SHARED_SLOT);
	p = plan(0x2, 0x6, 0x2);
	check("0x2to0x6_kind", p.kind, MT7925_MLO_PT_HIGHBAND_SHARED_SLOT);

	/* 3-slot hold (0x7): every pair change is a pure activation, including
	 * the shared-slot high-band entry
	 */
	p = plan(0x4, 0x5, 0x7);
	check("held7_0x4to0x5_kind", p.kind, MT7925_MLO_PT_ALREADY_HELD);
	p = plan(0x2, 0x3, 0x7);
	check("held7_0x2to0x3_kind", p.kind, MT7925_MLO_PT_ALREADY_HELD);
	p = plan(0x1, 0x3, 0x7);
	check("held7_0x1to0x3_kind", p.kind, MT7925_MLO_PT_ALREADY_HELD);
	p = plan(0x1, 0x5, 0x7);
	check("held7_0x1to0x5_kind", p.kind, MT7925_MLO_PT_ALREADY_HELD);
	p = plan(0x2, 0x6, 0x7);
	check("held7_0x2to0x6_kind", p.kind, MT7925_MLO_PT_ALREADY_HELD);
	/* survivor not held: still not eligible */
	p = plan(0x4, 0x5, 0x1);
	check("added_only_held_kind", p.kind, MT7925_MLO_PT_FULL_JOIN);

	/* nothing held (initial association): legacy two-link JOIN */
	p = plan(0x4, 0x6, 0);
	check("initial_0x4to0x6_kind", p.kind, MT7925_MLO_PT_FULL_JOIN);
	p = plan(0x4, 0x5, 0);
	check("nothing_held_0x4to0x5_kind", p.kind, MT7925_MLO_PT_FULL_JOIN);

	/* held slots that are not the survivor's do not count as survivor-held */
	p = plan(0x4, 0x5, 0x2);
	check("held_other_link_kind", p.kind, MT7925_MLO_PT_FULL_JOIN);

	/* malformed / no-op shapes never produce a delta acquisition */
	p = plan(0x5, 0x5, 0x5);
	check("noop_kind", p.kind, MT7925_MLO_PT_NONE);
	p = plan(0x6, 0x5, 0x6);
	check("swap_in_one_call_kind", p.kind, MT7925_MLO_PT_FULL_JOIN);
	p = plan(0x4, 0x5, 0x4);
	mt7925_mlo_pt_plan(0x4, 0x15, 0x7, 0x4, bands, &p);
	check("link_outside_valid_kind", p.kind, MT7925_MLO_PT_FULL_JOIN);

	/* grant validation for the delta acquisition (token 5, bss2, slot0) */
	check("grant_ok", mt7925_mlo_pt_grant_check(5, 2, 0, 5, 2, 0, 0),
	      MT7925_MLO_PT_GRANT_OK);
	check("grant_stale_token", mt7925_mlo_pt_grant_check(5, 2, 0, 4, 2, 0, 0),
	      MT7925_MLO_PT_GRANT_STALE_TOKEN);
	check("grant_wrong_bss", mt7925_mlo_pt_grant_check(5, 2, 0, 5, 0, 0, 0),
	      MT7925_MLO_PT_GRANT_WRONG_BSS);
	check("grant_wrong_slot", mt7925_mlo_pt_grant_check(5, 2, 0, 5, 2, 1, 0),
	      MT7925_MLO_PT_GRANT_WRONG_SLOT);
	check("grant_fw_status", mt7925_mlo_pt_grant_check(5, 2, 0, 5, 2, 0, 3),
	      MT7925_MLO_PT_GRANT_FW_STATUS);
	/* token 0 is never a live transaction */
	check("grant_zero_token", mt7925_mlo_pt_grant_check(0, 2, 0, 0, 2, 0, 0),
	      MT7925_MLO_PT_GRANT_STALE_TOKEN);

	/* per-link slot ownership: survivor keeps its old token, the added
	 * link is owned by the delta token, and releases go to the owner.
	 */
	st.held_token = 3;
	st.held_links = 0x4;
	st.link_token[2] = 3;
	check("owner_survivor", mt7925_mlo_roc_state_owner_token(&st, 2), 3);
	check("delta_grant_ret", mt7925_mlo_roc_state_delta_grant(&st, 5, 0), 1);
	check("owner_added", mt7925_mlo_roc_state_owner_token(&st, 0), 5);
	check("owner_survivor_unchanged", mt7925_mlo_roc_state_owner_token(&st, 2), 3);
	check("held_after_delta", st.held_links, 0x5);
	check("held_token_kept", st.held_token, 3);

	/* release the old survivor slot only: returns its owner token */
	mask = mt7925_mlo_roc_state_release_link(&st, 2, &tok);
	check("release_link_mask", mask, 0x4);
	check("release_link_token", tok, 3);
	check("held_after_release", st.held_links, 0x1);
	check("owner_added_after_release", mt7925_mlo_roc_state_owner_token(&st, 0), 5);
	/* releasing a link nobody holds is a no-op */
	mask = mt7925_mlo_roc_state_release_link(&st, 2, &tok);
	check("release_unheld_mask", mask, 0);
	/* release the delta-owned link: token 5 */
	mask = mt7925_mlo_roc_state_release_link(&st, 0, &tok);
	check("release_delta_mask", mask, 0x1);
	check("release_delta_token", tok, 5);
	check("held_empty", st.held_links, 0);
	check("held_token_dropped", st.held_token, 0);

	/* a delta grant on top of nothing held (defensive) still records it */
	check("delta_grant_empty", mt7925_mlo_roc_state_delta_grant(&st, 7, 1), 1);
	check("delta_owner", mt7925_mlo_roc_state_owner_token(&st, 1), 7);
	check("delta_held", st.held_links, 0x2);

	/* transition state machine: legal and illegal edges */
	check("sm_idle_prepare", mt7925_mlo_pt_can_advance(MT7925_MLO_PT_IDLE,
			MT7925_MLO_PT_PREPARE), 1);
	check("sm_acquire_admit", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_ACQUIRE_REPLACEMENT,
			MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER), 1);
	check("sm_acquire_failsafe", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_ACQUIRE_REPLACEMENT,
			MT7925_MLO_PT_FAILED_SAFE), 1);
	check("sm_wait_rx_failsafe", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_WAIT_REPLACEMENT_RX,
			MT7925_MLO_PT_FAILED_SAFE), 1);
	check("sm_no_skip_to_converged", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_ACQUIRE_REPLACEMENT,
			MT7925_MLO_PT_CONVERGED), 0);
	check("sm_no_grant_skip_admit", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_PREPARE,
			MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER), 0);
	check("sm_converged_idle", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_CONVERGED, MT7925_MLO_PT_IDLE), 1);
	check("sm_failsafe_idle", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_FAILED_SAFE, MT7925_MLO_PT_IDLE), 1);
	check("sm_failsafe_no_converge", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_FAILED_SAFE, MT7925_MLO_PT_CONVERGED), 0);

	/* survivor ownership handoff (release -> re-JOIN -> recover -> secure RX
	 * -> only then the incoming acquire): plan gating, states, recovery
	 * decision. Default (param off) must keep the existing path.
	 */
	check("handoff_wanted_delta_held", mt7925_mlo_pt_survivor_handoff_wanted(
			MT7925_MLO_PT_DELTA_ACQUIRE, 1, 0x4), 1);
	check("handoff_off_by_default_param", mt7925_mlo_pt_survivor_handoff_wanted(
			MT7925_MLO_PT_DELTA_ACQUIRE, 0, 0x4), 0);
	check("handoff_not_for_full_join", mt7925_mlo_pt_survivor_handoff_wanted(
			MT7925_MLO_PT_FULL_JOIN, 1, 0x4), 0);
	check("handoff_not_for_shared_slot", mt7925_mlo_pt_survivor_handoff_wanted(
			MT7925_MLO_PT_HIGHBAND_SHARED_SLOT, 1, 0x4), 0);
	check("handoff_needs_held_survivor", mt7925_mlo_pt_survivor_handoff_wanted(
			MT7925_MLO_PT_DELTA_ACQUIRE, 1, 0), 0);

	check("sm_preserve_to_handoff", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_PRESERVE_SURVIVOR,
			MT7925_MLO_PT_PREPARE_SURVIVOR_HANDOFF), 1);
	check("sm_preserve_to_acquire_still_ok", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_PRESERVE_SURVIVOR,
			MT7925_MLO_PT_ACQUIRE_REPLACEMENT), 1);
	check("sm_handoff_to_release", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_PREPARE_SURVIVOR_HANDOFF,
			MT7925_MLO_PT_RELEASE_SURVIVOR_OWNERSHIP), 1);
	check("sm_release_to_rejoin", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_RELEASE_SURVIVOR_OWNERSHIP,
			MT7925_MLO_PT_REJOIN_SURVIVOR), 1);
	/* after the fresh grant the survivor's firmware context is rebuilt
	 * (DEV/BSS/BMC) BEFORE the peer lifecycle stages run
	 */
	check("sm_rejoin_to_reprogram", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_REJOIN_SURVIVOR,
			MT7925_MLO_PT_REPROGRAM_SURVIVOR_CONTEXT), 1);
	check("sm_rejoin_no_direct_recover", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_REJOIN_SURVIVOR,
			MT7925_MLO_PT_RECOVER_SURVIVOR), 0);
	check("sm_reprogram_to_recover", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_REPROGRAM_SURVIVOR_CONTEXT,
			MT7925_MLO_PT_RECOVER_SURVIVOR), 1);
	check("sm_reprogram_to_wait_rx", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_REPROGRAM_SURVIVOR_CONTEXT,
			MT7925_MLO_PT_WAIT_SURVIVOR_RX), 1);
	check("sm_reprogram_failsafe", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_REPROGRAM_SURVIVOR_CONTEXT,
			MT7925_MLO_PT_FAILED_SAFE), 1);
	check("sm_no_acquire_from_reprogram", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_REPROGRAM_SURVIVOR_CONTEXT,
			MT7925_MLO_PT_ACQUIRE_REPLACEMENT), 0);
	check("sm_rejoin_to_wait_rx_no_recovery_needed", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_REJOIN_SURVIVOR,
			MT7925_MLO_PT_WAIT_SURVIVOR_RX), 1);
	check("sm_recover_to_wait_rx", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_RECOVER_SURVIVOR,
			MT7925_MLO_PT_WAIT_SURVIVOR_RX), 1);
	check("sm_wait_survivor_rx_to_acquire", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_WAIT_SURVIVOR_RX,
			MT7925_MLO_PT_ACQUIRE_REPLACEMENT), 1);
	/* the incoming link is never requested before survivor secure RX */
	check("sm_no_acquire_from_rejoin", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_REJOIN_SURVIVOR,
			MT7925_MLO_PT_ACQUIRE_REPLACEMENT), 0);
	check("sm_no_acquire_from_recover", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_RECOVER_SURVIVOR,
			MT7925_MLO_PT_ACQUIRE_REPLACEMENT), 0);
	check("sm_no_acquire_from_release", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_RELEASE_SURVIVOR_OWNERSHIP,
			MT7925_MLO_PT_ACQUIRE_REPLACEMENT), 0);
	check("sm_rejoin_failsafe", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_REJOIN_SURVIVOR,
			MT7925_MLO_PT_FAILED_SAFE), 1);
	check("sm_wait_survivor_rx_failsafe", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_WAIT_SURVIVOR_RX,
			MT7925_MLO_PT_FAILED_SAFE), 1);
	check("sm_wait_survivor_rx_no_converge", mt7925_mlo_pt_can_advance(
			MT7925_MLO_PT_WAIT_SURVIVOR_RX,
			MT7925_MLO_PT_CONVERGED), 0);

	/* recovery decision: valid RX seen >= settle time after the release
	 * proves the survivor kept its security state; frames right after the
	 * release (before firmware's cm=1 flip ~25 ms) prove nothing; none = broken
	 */
	check("recover_needed_no_valid_rx", mt7925_mlo_pt_survivor_needs_recovery(-1), 1);
	check("recover_needed_pre_cm_flip_frame", mt7925_mlo_pt_survivor_needs_recovery(20), 1);
	check("recover_not_needed_late_valid_rx", mt7925_mlo_pt_survivor_needs_recovery(300), 0);
	check("recover_boundary", mt7925_mlo_pt_survivor_needs_recovery(
			MT7925_MLO_PT_CM_SETTLE_MS), 0);

	/* bounded recovery escalation: rebuild the survivor's firmware context
	 * (DEV/BSS/BMC keys) first, then the peer recreate lifecycle, then the
	 * remove-first rebuild lifecycle, then fail safe (never open-ended)
	 */
	check("stage_first", mt7925_mlo_pt_recover_next(MT7925_MLO_PT_RECOVER_NONE, 0),
	      MT7925_MLO_PT_RECOVER_CONTEXT);
	check("stage_context_ok", mt7925_mlo_pt_recover_next(MT7925_MLO_PT_RECOVER_CONTEXT, 1),
	      MT7925_MLO_PT_RECOVER_DONE);
	check("stage_context_fail_escalates", mt7925_mlo_pt_recover_next(
			MT7925_MLO_PT_RECOVER_CONTEXT, 0), MT7925_MLO_PT_RECOVER_RECREATE);
	check("stage_recreate_ok", mt7925_mlo_pt_recover_next(MT7925_MLO_PT_RECOVER_RECREATE, 1),
	      MT7925_MLO_PT_RECOVER_DONE);
	check("stage_recreate_fail_escalates", mt7925_mlo_pt_recover_next(
			MT7925_MLO_PT_RECOVER_RECREATE, 0), MT7925_MLO_PT_RECOVER_REBUILD);
	check("stage_rebuild_ok", mt7925_mlo_pt_recover_next(MT7925_MLO_PT_RECOVER_REBUILD, 1),
	      MT7925_MLO_PT_RECOVER_DONE);
	check("stage_rebuild_fail_is_final", mt7925_mlo_pt_recover_next(
			MT7925_MLO_PT_RECOVER_REBUILD, 0), MT7925_MLO_PT_RECOVER_FAILED);

	/* survivor firmware-context reprogram order, taken from the normal
	 * lifecycle (mt7925_mac_link_bss_add -> bss info -> keys): DEV_INFO,
	 * then BSS_INFO (BASIC/MLD/RLM channel binding), then BMC group keys;
	 * a failing step stops the sequence (no further MCU commands)
	 */
	check("ctx_first_is_dev", mt7925_mlo_pt_ctx_next(MT7925_MLO_PT_CTX_NONE, 0),
	      MT7925_MLO_PT_CTX_DEV);
	check("ctx_dev_then_bss", mt7925_mlo_pt_ctx_next(MT7925_MLO_PT_CTX_DEV, 0),
	      MT7925_MLO_PT_CTX_BSS);
	check("ctx_bss_then_bmc_keys", mt7925_mlo_pt_ctx_next(MT7925_MLO_PT_CTX_BSS, 0),
	      MT7925_MLO_PT_CTX_BMC_KEYS);
	check("ctx_keys_then_done", mt7925_mlo_pt_ctx_next(MT7925_MLO_PT_CTX_BMC_KEYS, 0),
	      MT7925_MLO_PT_CTX_DONE);
	check("ctx_dev_err_stops", mt7925_mlo_pt_ctx_next(MT7925_MLO_PT_CTX_DEV, -5),
	      MT7925_MLO_PT_CTX_FAILED);
	check("ctx_bss_err_stops", mt7925_mlo_pt_ctx_next(MT7925_MLO_PT_CTX_BSS, -110),
	      MT7925_MLO_PT_CTX_FAILED);
	check("ctx_keys_err_stops", mt7925_mlo_pt_ctx_next(MT7925_MLO_PT_CTX_BMC_KEYS, -1),
	      MT7925_MLO_PT_CTX_FAILED);
	check("ctx_done_is_terminal", mt7925_mlo_pt_ctx_next(MT7925_MLO_PT_CTX_DONE, 0),
	      MT7925_MLO_PT_CTX_DONE);
	check("stage_names", (unsigned int)(mt7925_mlo_pt_state_name(
			MT7925_MLO_PT_REJOIN_SURVIVOR)[0] == 'R'), 1);

	/* Sep-18 replay (test-only): after the historical full two-TLV JOIN is
	 * granted, the survivor gets the peer-only recovery ladder (recreate,
	 * then rebuild) -- no DEV/BSS/BMC context stage, because the JOIN itself
	 * re-granted the channel binding. It runs only when the replay is on and
	 * the survivor did not already resume secure RX after the grant.
	 */
	check("sep18_peer_first_is_recreate", mt7925_mlo_pt_peer_recover_next(
			MT7925_MLO_PT_RECOVER_NONE, 0), MT7925_MLO_PT_RECOVER_RECREATE);
	check("sep18_peer_recreate_ok", mt7925_mlo_pt_peer_recover_next(
			MT7925_MLO_PT_RECOVER_RECREATE, 1), MT7925_MLO_PT_RECOVER_DONE);
	check("sep18_peer_recreate_fail_rebuild", mt7925_mlo_pt_peer_recover_next(
			MT7925_MLO_PT_RECOVER_RECREATE, 0), MT7925_MLO_PT_RECOVER_REBUILD);
	check("sep18_peer_rebuild_ok", mt7925_mlo_pt_peer_recover_next(
			MT7925_MLO_PT_RECOVER_REBUILD, 1), MT7925_MLO_PT_RECOVER_DONE);
	check("sep18_peer_rebuild_fail_final", mt7925_mlo_pt_peer_recover_next(
			MT7925_MLO_PT_RECOVER_REBUILD, 0), MT7925_MLO_PT_RECOVER_FAILED);
	check("sep18_peer_never_context", mt7925_mlo_pt_peer_recover_next(
			MT7925_MLO_PT_RECOVER_NONE, 1) == MT7925_MLO_PT_RECOVER_CONTEXT, 0);
	check("sep18_recover_off_never", mt7925_mlo_pt_sep18_recovery_wanted(0, 0), 0);
	check("sep18_recover_skipped_if_rx_resumed",
	      mt7925_mlo_pt_sep18_recovery_wanted(1, 1), 0);
	check("sep18_recover_runs_if_no_rx", mt7925_mlo_pt_sep18_recovery_wanted(1, 0), 1);

	/* Post-regrant security rebind (mlo_security_rebind): a radio-group
	 * reincarnation (old privilege released + full JOIN re-granted) makes
	 * the firmware-local security binding of BOTH active links stale --
	 * the survivor is not exempt.
	 */
	check("rebind_off_no_links", mt7925_mlo_pt_rebind_links(0, 0x5), 0);
	check("rebind_reincarnated_both", mt7925_mlo_pt_rebind_links(1, 0x5), 0x5);
	check("rebind_includes_survivor", mt7925_mlo_pt_rebind_links(1, 0x5) & 0x4, 0x4);
	check("rebind_includes_replacement", mt7925_mlo_pt_rebind_links(1, 0x5) & 0x1, 0x1);
	/* stale hw_key_idx (a valid-looking index) must not suppress PTK/GTK
	 * reprogramming once the incarnation changed
	 */
	check("key_stale_idx_reprogram", mt7925_mlo_pt_key_reprogram_needed(0, 1), 1);
	check("key_valid_idx_no_reincarnation", mt7925_mlo_pt_key_reprogram_needed(0, 0), 0);
	check("key_unset_always_reprogram", mt7925_mlo_pt_key_reprogram_needed(255, 0), 1);
	/* STA_REC_MLD record membership comes from the post-grant target set,
	 * not the not-yet-published mac80211 active set
	 */
	check("mld_uses_target_when_stale_active", mt7925_mlo_pt_mld_links(0x4, 0x5), 0x5);
	check("mld_no_target_uses_active", mt7925_mlo_pt_mld_links(0x6, 0), 0x6);
	/* fixed security reconstruction order; a failing phase stops it */
	check("rebind_order_invalidate_first", mt7925_mlo_pt_rebind_next(
			MT7925_MLO_PT_REBIND_NONE, 0), MT7925_MLO_PT_REBIND_INVALIDATE);
	check("rebind_order_sta_rec", mt7925_mlo_pt_rebind_next(
			MT7925_MLO_PT_REBIND_INVALIDATE, 0), MT7925_MLO_PT_REBIND_STA_REC);
	check("rebind_order_ptk", mt7925_mlo_pt_rebind_next(
			MT7925_MLO_PT_REBIND_STA_REC, 0), MT7925_MLO_PT_REBIND_PTK);
	check("rebind_order_gtk", mt7925_mlo_pt_rebind_next(
			MT7925_MLO_PT_REBIND_PTK, 0), MT7925_MLO_PT_REBIND_GTK);
	check("rebind_order_authorize", mt7925_mlo_pt_rebind_next(
			MT7925_MLO_PT_REBIND_GTK, 0), MT7925_MLO_PT_REBIND_AUTHORIZE);
	check("rebind_order_wait_rx", mt7925_mlo_pt_rebind_next(
			MT7925_MLO_PT_REBIND_AUTHORIZE, 0), MT7925_MLO_PT_REBIND_WAIT_RX);
	check("rebind_order_done", mt7925_mlo_pt_rebind_next(
			MT7925_MLO_PT_REBIND_WAIT_RX, 0), MT7925_MLO_PT_REBIND_DONE);
	check("rebind_err_fails", mt7925_mlo_pt_rebind_next(
			MT7925_MLO_PT_REBIND_PTK, -5), MT7925_MLO_PT_REBIND_FAILED);
	/* CONVERGED only when BOTH links have security-valid RX */
	check("converge_none_secure", mt7925_mlo_pt_converge_allowed(0x5, 0), 0);
	check("converge_survivor_only", mt7925_mlo_pt_converge_allowed(0x5, 0x4), 0);
	check("converge_replacement_only", mt7925_mlo_pt_converge_allowed(0x5, 0x1), 0);
	check("converge_both", mt7925_mlo_pt_converge_allowed(0x5, 0x5), 1);
	check("converge_empty_set_never", mt7925_mlo_pt_converge_allowed(0, 0), 0);

	/* Post-regrant parent (DEV/BSS) rebuild (mlo_regrant_parent_rebuild):
	 * after a full radio-group regrant BOTH links' DEV_INFO and BSS_INFO are
	 * rebuilt against the new grants, parent before child, before any
	 * STA_REC/key/authorize step. The survivor never inherits its old parent.
	 */
	check("parent_off_no_links", mt7925_mlo_pt_parent_links(0, 0x5), 0);
	check("parent_both_links", mt7925_mlo_pt_parent_links(1, 0x5), 0x5);
	check("parent_includes_survivor", mt7925_mlo_pt_parent_links(1, 0x5) & 0x4, 0x4);
	check("parent_includes_replacement", mt7925_mlo_pt_parent_links(1, 0x5) & 0x1, 0x1);
	check("parent_order_dev_first", mt7925_mlo_pt_parent_next(
			MT7925_MLO_PT_PARENT_NONE, 0), MT7925_MLO_PT_PARENT_DEV);
	check("parent_order_bss", mt7925_mlo_pt_parent_next(
			MT7925_MLO_PT_PARENT_DEV, 0), MT7925_MLO_PT_PARENT_BSS);
	check("parent_order_done", mt7925_mlo_pt_parent_next(
			MT7925_MLO_PT_PARENT_BSS, 0), MT7925_MLO_PT_PARENT_DONE);
	check("parent_err_fails", mt7925_mlo_pt_parent_next(
			MT7925_MLO_PT_PARENT_DEV, -5), MT7925_MLO_PT_PARENT_FAILED);
	/* child security phases may start only after the parent pass is DONE */
	check("child_blocked_parent_pending", mt7925_mlo_pt_child_allowed(1,
			MT7925_MLO_PT_PARENT_BSS), 0);
	check("child_blocked_parent_failed", mt7925_mlo_pt_child_allowed(1,
			MT7925_MLO_PT_PARENT_FAILED), 0);
	check("child_allowed_parent_done", mt7925_mlo_pt_child_allowed(1,
			MT7925_MLO_PT_PARENT_DONE), 1);
	check("child_allowed_parent_off", mt7925_mlo_pt_child_allowed(0,
			MT7925_MLO_PT_PARENT_NONE), 1);
	{
		/* full ordered sequence; old ordering (survivor parent inherited,
		 * security first) must be rejected
		 */
		static const int good[] = {
			MT7925_MLO_PT_SEQ_DEV, MT7925_MLO_PT_SEQ_BSS,
			MT7925_MLO_PT_SEQ_STA_REC, MT7925_MLO_PT_SEQ_STA_REC_MLD,
			MT7925_MLO_PT_SEQ_KEYS, MT7925_MLO_PT_SEQ_AUTHORIZE,
			MT7925_MLO_PT_SEQ_RX_GATE,
		};
		static const int old_order[] = {
			MT7925_MLO_PT_SEQ_STA_REC, MT7925_MLO_PT_SEQ_KEYS,
			MT7925_MLO_PT_SEQ_STA_REC_MLD, MT7925_MLO_PT_SEQ_AUTHORIZE,
			MT7925_MLO_PT_SEQ_RX_GATE,
		};
		static const int bss_before_dev[] = {
			MT7925_MLO_PT_SEQ_BSS, MT7925_MLO_PT_SEQ_DEV,
			MT7925_MLO_PT_SEQ_STA_REC, MT7925_MLO_PT_SEQ_STA_REC_MLD,
			MT7925_MLO_PT_SEQ_KEYS, MT7925_MLO_PT_SEQ_AUTHORIZE,
			MT7925_MLO_PT_SEQ_RX_GATE,
		};
		static const int keys_before_parent[] = {
			MT7925_MLO_PT_SEQ_DEV, MT7925_MLO_PT_SEQ_KEYS,
			MT7925_MLO_PT_SEQ_BSS, MT7925_MLO_PT_SEQ_STA_REC,
			MT7925_MLO_PT_SEQ_STA_REC_MLD, MT7925_MLO_PT_SEQ_AUTHORIZE,
			MT7925_MLO_PT_SEQ_RX_GATE,
		};

		check("seq_parent_first_ok", mt7925_mlo_pt_regrant_seq_ok(good,
				sizeof(good) / sizeof(good[0])), 1);
		check("seq_inherited_parent_rejected", mt7925_mlo_pt_regrant_seq_ok(
				old_order, sizeof(old_order) / sizeof(old_order[0])), 0);
		check("seq_bss_before_dev_rejected", mt7925_mlo_pt_regrant_seq_ok(
				bss_before_dev,
				sizeof(bss_before_dev) / sizeof(bss_before_dev[0])), 0);
		check("seq_keys_before_parent_rejected", mt7925_mlo_pt_regrant_seq_ok(
				keys_before_parent,
				sizeof(keys_before_parent) / sizeof(keys_before_parent[0])), 0);
	}

	/* stale completions never mutate a newer transition */
	check("gen_current", mt7925_mlo_pt_event_is_current(9, 9), 1);
	check("gen_stale", mt7925_mlo_pt_event_is_current(8, 9), 0);

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures ? 1 : 0;
}
