#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Audit mt7927_mlo_change_sta_links() against the same mandatory 2->1->2
# mac80211 sequence as mt7927_mlo_change_vif_links(). Unlike the vif-links
# callback, this one has no hweight()-based gate on new_links at all, so it
# already tolerates a single-bit new_links (rem-only) or growth from a
# single-bit old_links -- this contract pins that down so it cannot
# regress, rather than fixing a proven break.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'sta-links shrink/grow contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^int mt7927_mlo_change_sta_links(/,/^}/p' mt7925/mlo/lifecycle.c)
[[ -n $body ]] || fail "mt7927_mlo_change_sta_links is missing"

# 1. Must not reject a single-bit new_links or old_links -- that is the
#    mandatory intermediate survivor state, not an error.
if rg -q 'hweight16\(new_links\)\s*!=\s*2|hweight16\(old_links\)\s*!=\s*2' <<< "$body"; then
	fail "change_sta_links rejects a non-two-link mask -- this is a mandatory intermediate mac80211 state, not an error"
fi

# 2. Must never evaluate __ffs() on a mask that can be empty. This function
#    does not compute a secondary link id today -- pin that down so a future
#    edit does not reintroduce the same undefined-__ffs(0) class of bug
#    fixed in mt7927_mlo_change_vif_links().
if rg -q '__ffs\([a-z_]*links & ~BIT\(' <<< "$body"; then
	fail "change_sta_links evaluates __ffs() on a mask that can be empty for a single-link transition"
fi

# 3. Bookkeeping (valid_links / traffic_active_links) must be committed
#    strictly after both the rem and add loops, i.e. after all per-link
#    admission has already succeeded -- any failure inside either loop must
#    reach its own goto before this line, never fall through into it.
#    The activation add loop is the LAST for_each_set_bit(link_id, &add, ...)
#    in the function -- an earlier admitted_add computation loop
#    (mlo_sta_admission_gate_contract.sh) shares the same signature.
#    Bookkeeping now commits `refresh_links` (new_links minus any
#    ROC-unadmitted substitute), not raw new_links -- see
#    mlo_sta_admission_gate_contract.sh.
rem_loop_line=$(grep -n 'for_each_set_bit(link_id, &rem,' <<< "$body" | head -1 | cut -d: -f1 || true)
add_loop_line=$(grep -n 'for_each_set_bit(link_id, &add,' <<< "$body" | tail -1 | cut -d: -f1 || true)
commit_line=$(grep -n 'traffic_active_links = refresh_links' <<< "$body" | head -1 | cut -d: -f1 || true)

[[ -n $rem_loop_line && -n $add_loop_line && -n $commit_line ]] || fail "could not locate rem/add loops or commit line"

if (( commit_line < rem_loop_line || commit_line < add_loop_line )); then
	fail "traffic_active_links is committed (line $commit_line) before the rem (line $rem_loop_line) or add (line $add_loop_line) loop has completed"
fi

printf 'sta-links shrink/grow contract passed\n'
