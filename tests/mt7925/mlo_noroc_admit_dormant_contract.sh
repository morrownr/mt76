#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# test_mt7927_mlo_noroc_admit exists to measure what happens when a
# substitute link's ROC/JOIN times out and the failure is swallowed instead
# of torn down -- it is a diagnostic bypass of the *error return*, not proof
# that the substitute was actually admitted. Before this contract, bypassing
# the error also left the substitute's wcid.link_valid=true (set eagerly by
# mt7927_mlo_add_bss_link() on creation) and committed it into
# traffic_active_links regardless -- letting the TX/RX scheduler hand real
# traffic to a link firmware never granted airtime to. Proven live: the
# substitute's rx_pkts stayed 0 while tx_ac accumulated real airtime with no
# return traffic, and ping lost ~50s then settled to ~40% ongoing loss.
#
# The fix tracks whether the ROC call produced a REAL grant separately from
# whatever `err` gets forced to for mac80211's benefit, and gates both
# wcid.link_valid and the traffic_active_links commit on that real result --
# noroc_admit may suppress the *error*, it must never fake the *grant*.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'noroc_admit dormancy contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
[[ -n $body ]] || fail "mt7927_mlo_change_vif_links is missing"

completion_body=$(awk '/^\tif \(hweight16\(old_links\) == 1 && hweight16\(new_links\) == 2/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "$body")
[[ -n $completion_body ]] || fail "could not isolate completion-phase branch body"

# 1. A real-grant tracking variable must exist, distinct from `err`.
if ! rg -q 'real_grant' <<< "$completion_body"; then
	fail "no real_grant tracking distinct from err -- noroc_admit's error bypass would be indistinguishable from an actual grant"
fi

# 2. The noroc_admit bypass branch must not set real_grant true.
noroc_block=$(awk '/mt7925_test_mt7927_mlo_noroc_admit/{flag=1} flag{print} flag && /^\t\t\}/{exit}' <<< "$completion_body")
[[ -n $noroc_block ]] || fail "could not isolate the noroc_admit bypass block"
if rg -q 'real_grant = true' <<< "$noroc_block"; then
	fail "noroc_admit bypass block sets real_grant = true -- it must only suppress the error, never fake the grant"
fi

# 3. The final commit (wcid.link_valid = true and traffic_active_links =
#    new_links for the completed pair) must be unreachable unless
#    real_grant is true -- i.e. gated behind a real_grant check, not merely
#    behind err == 0.
commit_line=$(grep -n 'traffic_active_links = new_links' <<< "$completion_body" | head -1 | cut -d: -f1)
[[ -n $commit_line ]] || fail "could not find the pair-commit line"

guard_line=$(grep -n 'real_grant' <<< "$completion_body" | grep -E '^\s*[0-9]+:.*if' | tail -1 | cut -d: -f1)
if [[ -z $guard_line ]]; then
	fail "no 'if (...real_grant...)' guard found before the pair commit"
fi
if (( guard_line > commit_line )); then
	fail "real_grant guard (line $guard_line) appears after the pair commit (line $commit_line) -- commit is not actually gated"
fi

# 4. The substitute's wcid must not be marked traffic-eligible
#    (link_valid = true) anywhere in this branch except behind that same
#    real_grant-gated commit -- mt7927_mlo_add_bss_link()'s eager
#    activation must be explicitly undone for this phase.
if ! rg -q 'wcid\.link_valid = false' <<< "$completion_body"; then
	fail "completion phase never reverts the substitute's eager wcid.link_valid=true from mt7927_mlo_add_bss_link()"
fi

printf 'noroc_admit dormancy contract passed\n'
