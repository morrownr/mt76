#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# The VIF completion phase (mt7927_mlo_change_vif_links(), old_links==1 &&
# new_links==2) already keeps traffic_active_links honest on ROC failure
# (mlo_noroc_admit_dormant_contract.sh), but the immediately-following STA
# callback (mt7927_mlo_change_sta_links()) had no way to learn that decision
# -- it unconditionally refreshed/activated every bit in new_links. Proven
# live: after a real ROC timeout with real_grant=0, wcid=3 (the substitute)
# still showed valid=1 in link_stats, because mt7927_mlo_change_sta_links()
# ran mt7927_mlo_refresh_reassigned_sta() and its own add loop against the
# raw new_links regardless.
#
# This contract pins down the explicit, per-link admission state the two
# callbacks now share -- never inferred from callback ordering, new_links,
# `created`, link_ready, or the transient global phy->roc_grant (which is
# already consumed/reset by the time the STA callback runs).

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'ROC admission-state contract failed: %s\n' "$1" >&2
	exit 1
}

# 1. Explicit per-link admission state must exist.
if ! rg -q 'bool roc_admitted;' mt792x.h; then
	fail "struct mt7927_mlo_link_state has no explicit roc_admitted field"
fi
if ! rg -q 'u32 admitted_generation;' mt792x.h; then
	fail "struct mt7927_mlo_link_state has no admitted_generation field for freshness checking"
fi

body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
[[ -n $body ]] || fail "mt7927_mlo_change_vif_links is missing"

completion_body=$(awk '/^\tif \(hweight16\(old_links\) == 1 && hweight16\(new_links\) == 2/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "$body")
[[ -n $completion_body ]] || fail "could not isolate completion-phase branch body"

# 2a. Substitute admission must be cleared before requesting ROC.
clear_line=$(grep -n 'roc_admitted = false' <<< "$completion_body" | head -1 | cut -d: -f1 || true)
roc_call_line=$(grep -n 'mt7925_set_mlo_roc(phy, anchor_mconf, new_links)' <<< "$completion_body" | head -1 | cut -d: -f1 || true)
[[ -n $clear_line ]] || fail "completion phase never clears roc_admitted for the substitute"
[[ -n $roc_call_line ]] || fail "completion phase does not call mt7925_set_mlo_roc(phy, anchor_mconf, new_links)"
if (( clear_line > roc_call_line )); then
	fail "roc_admitted is cleared (line $clear_line) after the ROC call (line $roc_call_line), not before"
fi

# 2b. Admission must be set true only behind the real_grant gate (not merely
#     err == 0, which noroc_admit can also produce).
if ! rg -q 'real_grant' <<< "$completion_body"; then
	fail "no real_grant tracking -- required to distinguish a genuine grant from a noroc_admit bypass"
fi
grant_set_line=$(grep -n 'roc_admitted = true' <<< "$completion_body" | head -1 | cut -d: -f1 || true)
[[ -n $grant_set_line ]] || fail "completion phase never sets roc_admitted = true on success"

real_grant_guard_line=$(grep -Fn 'if (!real_grant)' <<< "$completion_body" | head -1 | cut -d: -f1 || true)
[[ -n $real_grant_guard_line ]] || fail "no 'if (!real_grant)' early-return guard found"
if (( grant_set_line < real_grant_guard_line )); then
	fail "roc_admitted = true (line $grant_set_line) is set before the real_grant check (line $real_grant_guard_line) -- must be gated behind it"
fi

# 2c. Admitted generation must be stamped alongside roc_admitted = true
#     (allow the assignment to wrap across lines).
admit_block=$(awk -v n="$grant_set_line" 'NR>=n-2 && NR<=n+4' <<< "$completion_body")
if ! rg -Fq 'admitted_generation =' <<< "$admit_block"; then
	fail "roc_admitted = true is not paired with stamping admitted_generation"
fi
if ! rg -Fq 'mvif->mt7927_mlo.transition_generation' <<< "$admit_block"; then
	fail "admitted_generation is not stamped from the current transition_generation"
fi

# 2d. The shrink phase (Phase 1) and the generic rem loop must clear
#     admission when a link is retired.
shrink_body=$(awk '/^\tif \(hweight16\(new_links\) == 1\) \{/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "${body#*Phase 1: shrink}")
[[ -n $shrink_body ]] || fail "could not isolate shrink-phase (Phase 1) branch body"
if ! rg -q 'roc_admitted = false' <<< "$shrink_body"; then
	fail "shrink phase does not clear roc_admitted for the retired link"
fi

printf 'ROC admission-state contract passed\n'
