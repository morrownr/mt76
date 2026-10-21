#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# The standalone SUB_LINK ROC experiment must not perturb the normal
# full-MLO JOIN GRANT path (UNI_EVENT_ROC_GRANT, tag=0) or initial
# association. Pin both down explicitly so the experimental addition can
# never silently change proven-working behavior.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'sublink ROC unchanged-paths contract failed: %s\n' "$1" >&2
	exit 1
}

grant_handler=$(sed -n '/^static void mt7925_mcu_roc_handle_grant(/,/^}/p' mt7925/mcu.c)
[[ -n $grant_handler ]] || fail "mt7925_mcu_roc_handle_grant is missing"

# 1. The full-JOIN GRANT handler must still set roc_grant/wake roc_wait
#    exactly as before -- untouched by the sublink experiment.
if ! rg -Fq 'dev->phy.roc_grant = true;' <<< "$grant_handler"; then
	fail "mt7925_mcu_roc_handle_grant no longer sets phy.roc_grant -- full-JOIN completion path regressed"
fi
if ! rg -Fq 'wake_up(&dev->phy.roc_wait);' <<< "$grant_handler"; then
	fail "mt7925_mcu_roc_handle_grant no longer wakes roc_wait -- full-JOIN completion path regressed"
fi

# 2. The full-JOIN GRANT handler must not reference the new dedicated
#    sublink state at all -- the two completion paths must stay fully
#    independent.
if rg -q 'sublink_roc' <<< "$grant_handler"; then
	fail "mt7925_mcu_roc_handle_grant (tag=0 path) references sublink_roc state -- the two completion paths must stay independent"
fi

# 3. mt7927_mlo_change_vif_links()'s Phase 2 (1->2 completion) must retain
#    the exact full-pair ROC call as the non-experimental default.
body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
completion_body=$(awk '/^\tif \(hweight16\(old_links\) == 1 && hweight16\(new_links\) == 2/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "$body")
[[ -n $completion_body ]] || fail "could not isolate completion-phase branch body"

if ! rg -Fq 'mt7925_set_mlo_roc(phy, anchor_mconf, new_links)' <<< "$completion_body"; then
	fail "completion phase no longer retains the full-pair ROC call for the non-experimental (flag-off) default"
fi

# 4. The experimental path must be gated behind the existing
#    test_mt7927_sublink_roc flag, read via READ_ONCE (matching this
#    driver's existing convention for every other runtime test toggle).
if ! rg -q 'READ_ONCE\(mt7925_test_mt7927_sublink_roc\)' <<< "$completion_body"; then
	fail "completion phase does not gate the sublink experiment behind READ_ONCE(mt7925_test_mt7927_sublink_roc)"
fi

# 5. Initial association never reaches mt7927_mlo_change_vif_links() at
#    all (old_links==0 fails mt7927_mlo_active_transition's old_links
#    check) -- pin that guard down so it can't regress into also gating
#    initial association.
if ! rg -q 'old_links && new_links &&' mt7925/mlo/lifecycle.c; then
	fail "mt7927_mlo_active_transition no longer requires old_links to be nonzero -- initial association (old_links==0) could now reach the experimental gate"
fi

printf 'sublink ROC unchanged-paths contract passed\n'
