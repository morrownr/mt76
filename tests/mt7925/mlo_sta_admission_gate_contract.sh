#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mt7927_mlo_change_sta_links() must never treat an add-loop link as
# traffic-eligible unless the VIF callback actually recorded a real ROC
# grant for it in the CURRENT transition generation. The critical ordering
# detail: mt7927_mlo_refresh_reassigned_sta() itself flips
# wcid.link_valid=true (via a real MCU STA update) for any link in its
# input mask whose phy_idx doesn't yet match its mconf's band_idx -- exactly
# the state an unadmitted substitute is left in right after the VIF
# callback's band reassignment. Gating only the later add-loop
# `link_valid = true` assignment is not enough; the substitute must already
# be excluded from the mask passed to refresh_reassigned_sta() itself.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'STA admission-gate contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^int mt7927_mlo_change_sta_links(/,/^}/p' mt7925/mlo/lifecycle.c)
[[ -n $body ]] || fail "mt7927_mlo_change_sta_links is missing"

# 1. An admitted-add mask must be computed from roc_admitted +
#    admitted_generation, not from new_links/created/link_ready alone.
if ! rg -q 'admitted_add' <<< "$body"; then
	fail "no admitted_add mask computed -- admission is not being consulted at all"
fi
if ! rg -q 'roc_admitted &&' <<< "$body"; then
	fail "admitted_add is not gated on roc_admitted"
fi
if ! rg -q 'admitted_generation == mvif->mt7927_mlo\.transition_generation' <<< "$body"; then
	fail "admitted_add does not check admitted_generation against the current transition_generation -- a stale delayed grant could authorize a re-added link"
fi

# 2. The mask passed into mt7927_mlo_refresh_reassigned_sta() must exclude
#    any unadmitted add-loop link -- computed and applied BEFORE that call,
#    not the raw new_links.
refresh_line=$(grep -Fn 'mt7927_mlo_refresh_reassigned_sta(' <<< "$body" | head -1 | cut -d: -f1 || true)
admitted_add_line=$(grep -Fn 'admitted_add' <<< "$body" | head -1 | cut -d: -f1 || true)
[[ -n $refresh_line ]] || fail "mt7927_mlo_refresh_reassigned_sta() is not called"
[[ -n $admitted_add_line ]] || fail "admitted_add is never computed"
if (( admitted_add_line > refresh_line )); then
	fail "admitted_add (line $admitted_add_line) is computed after mt7927_mlo_refresh_reassigned_sta() (line $refresh_line) -- must exclude the substitute before that call, not after"
fi
if rg -Fq 'mt7927_mlo_refresh_reassigned_sta(dev, vif, sta, msta, new_links)' <<< "$body"; then
	fail "mt7927_mlo_refresh_reassigned_sta() is still called with the raw, unfiltered new_links"
fi

# 3. The add loop must skip reconciliation/association/note_sta(true) for an
#    unadmitted link -- i.e. there must be a per-link admitted check inside
#    the add loop, guarding the activation sequence.
# The admitted_add computation loop earlier in the function has the same
# for_each_set_bit(link_id, &add, ...) signature -- skip to the SECOND
# occurrence, which is the real activation loop.
add_loop_body=$(awk '/for_each_set_bit\(link_id, &add,/{c++; if (c==2) flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "$body")
[[ -n $add_loop_body ]] || fail "could not isolate the add loop body"
if ! rg -q 'admitted_add & BIT\(link_id\)' <<< "$add_loop_body"; then
	fail "add loop does not check admitted_add per link_id before activating it"
fi

# 4. An unadmitted link must be forced dormant explicitly (WCID invalid,
#    not traffic-active) rather than merely skipped -- a stale prior state
#    must not be able to leave it looking eligible.
dormant_block=$(awk '/!\(admitted_add & BIT\(link_id\)\)/{flag=1} flag{print} flag && /continue;/{exit}' <<< "$add_loop_body")
[[ -n $dormant_block ]] || fail "could not isolate the not-admitted branch of the add loop"
if ! rg -q 'wcid\.link_valid = false' <<< "$dormant_block"; then
	fail "unadmitted substitute is not explicitly forced wcid.link_valid = false"
fi
if ! rg -q 'mt7927_mlo_note_sta\(mvif, mlink, false\)' <<< "$dormant_block"; then
	fail "unadmitted substitute's per-link traffic-active state is not explicitly cleared via mt7927_mlo_note_sta(..., false)"
fi

# 5. The reconcile/associate/note_sta(true) activation sequence must never
#    run for an unadmitted link -- it must live strictly after the
#    admitted_add check's early continue, not merely alongside it.
if ! rg -q 'mt7925_mlo_reconcile_link' <<< "$add_loop_body"; then
	fail "add loop no longer reconciles admitted links -- already-admitted links must remain unchanged"
fi
reconcile_line=$(grep -Fn 'mt7925_mlo_reconcile_link' <<< "$add_loop_body" | head -1 | cut -d: -f1 || true)
dormant_check_line=$(grep -Fn '!(admitted_add & BIT(link_id))' <<< "$add_loop_body" | head -1 | cut -d: -f1 || true)
[[ -n $reconcile_line ]] || fail "could not find mt7925_mlo_reconcile_link in the add loop"
[[ -n $dormant_check_line ]] || fail "could not find the admitted_add dormancy check inside the add loop"
if (( dormant_check_line > reconcile_line )); then
	fail "the admitted_add dormancy check (line $dormant_check_line) appears after reconciliation (line $reconcile_line) -- must gate before it"
fi

# 6. Final traffic_active_links/valid_links commit must exclude any
#    unadmitted substitute, not blindly assign new_links.
if rg -Fq 'mvif->mt7927_mlo.traffic_active_links = new_links;' <<< "$body"; then
	fail "traffic_active_links is still unconditionally committed to new_links, re-including any unadmitted substitute"
fi

printf 'STA admission-gate contract passed\n'
