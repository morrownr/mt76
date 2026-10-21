#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# The post-key-switch standalone-ROC experiment cannot attempt ROC from
# inside the VIF completion phase at all -- mac80211's own
# ieee80211_key_switch_links() (uncontrolled, internal, not exported) only
# runs strictly AFTER that phase's callback already returned. Instead the
# VIF phase records explicit pending state (peer-owned, per the STA-owned
# redesign: mt792x_vif caches no lifetime-safe peer pointer, so it must
# resolve the peer via a brief RCU-scoped ieee80211_find_sta() lookup) and
# returns; the SET_KEY trigger in .set_key() does the real work later.
# This contract pins the VIF-side half: what gets recorded, on which
# object, when, and that no ROC of either kind is attempted from here.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'postkey pending-state contract failed: %s\n' "$1" >&2
	exit 1
}

# 1. New flag exists, defaults false, registered as a module parameter.
#    Non-static (was static) so mt7925_set_key() in main.c can check it
#    for the SET_KEY trigger -- exposed the same way as
#    mt7925_mlo_group_key_rebind() earlier in this driver's history.
if ! rg -q '^bool mt7925_test_mt7927_sublink_postkey;' mt7925/mlo/lifecycle.c; then
	fail "no mt7925_test_mt7927_sublink_postkey flag declared (non-static, for cross-file use)"
fi
if ! rg -q 'module_param_named\(test_mt7927_sublink_postkey' mt7925/mlo/lifecycle.c; then
	fail "test_mt7927_sublink_postkey is not registered as a module parameter"
fi
if ! rg -Fq 'extern bool mt7925_test_mt7927_sublink_postkey;' mt7925/mlo/decls.h; then
	fail "mt7925_test_mt7927_sublink_postkey has no extern declaration in mlo/decls.h"
fi
if ! rg -Fq 'extern bool mt7925_test_mt7927_sublink_roc;' mt7925/mlo/decls.h; then
	fail "mt7925_test_mt7927_sublink_roc has no extern declaration in mlo/decls.h"
fi

# 2. Pending-state fields live on mt792x_sta (peer-owned), not
#    mt7927_mlo_state (VIF-owned) -- see mlo_postkey_sta_owned_work_contract.sh
#    for the struct-placement half of this; here we only check the VIF
#    completion phase writes the peer's fields, not its own.
body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
completion_body=$(awk '/^\tif \(hweight16\(old_links\) == 1 && hweight16\(new_links\) == 2/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "$body")
[[ -n $completion_body ]] || fail "could not isolate completion-phase branch body"

postkey_block=$(awk '/mt7925_test_mt7927_sublink_postkey/{flag=1} flag{print} flag && /^\t\t\}/{exit}' <<< "$completion_body")
[[ -n $postkey_block ]] || fail "no postkey-gated block found in the completion phase"

# 3. Requires the standalone-sublink experiment too (item 1).
if ! rg -q 'READ_ONCE\(mt7925_test_mt7927_sublink_roc\)' <<< "$postkey_block"; then
	fail "postkey block does not also require test_mt7927_sublink_roc"
fi

# 4. Resolves the peer via a brief RCU-scoped lookup and writes into the
#    peer's own pending fields (pmsta->postkey_*), not mvif's.
if ! rg -Fq 'rcu_read_lock();' <<< "$postkey_block"; then
	fail "postkey block does not take rcu_read_lock() to resolve the peer"
fi
if ! rg -Fq 'ieee80211_find_sta(vif, vif->cfg.ap_addr)' <<< "$postkey_block"; then
	fail "postkey block does not resolve the peer via ieee80211_find_sta(vif, vif->cfg.ap_addr)"
fi
if ! rg -Fq 'rcu_read_unlock();' <<< "$postkey_block"; then
	fail "postkey block does not release RCU after resolving the peer"
fi
if ! rg -Fq 'pmsta->postkey_pending = true' <<< "$postkey_block"; then
	fail "postkey block does not set pmsta->postkey_pending = true on the resolved peer"
fi
if ! rg -Fq 'pmsta->postkey_sublink_id = sub_id' <<< "$postkey_block"; then
	fail "postkey block does not record pmsta->postkey_sublink_id = sub_id"
fi
if ! rg -Fq 'pmsta->postkey_survivor_id = anchor_id' <<< "$postkey_block"; then
	fail "postkey block does not record pmsta->postkey_survivor_id = anchor_id"
fi
if ! rg -Fq 'pmsta->postkey_generation =' <<< "$postkey_block"; then
	fail "postkey block does not stamp pmsta->postkey_generation"
fi
if ! rg -Fq 'mvif->mt7927_mlo.transition_generation;' <<< "$postkey_block"; then
	fail "postkey block does not stamp pending_generation from the current transition_generation"
fi

# 5. Must not issue either ROC path from here -- no call to
#    mt7925_set_sublink_roc or mt7925_set_mlo_roc inside this block.
if rg -Fq 'mt7925_set_sublink_roc(' <<< "$postkey_block"; then
	fail "postkey block issues the standalone sublink ROC itself -- must defer to the SET_KEY trigger"
fi
if rg -Fq 'mt7925_set_mlo_roc(' <<< "$postkey_block"; then
	fail "postkey block issues the full-pair ROC -- must not fall back"
fi

# 6. Must return before reaching the existing ROC-attempt branches (the
#    sublink experimental branch and the full-pair default branch) --
#    i.e. a bare 'return 0;' inside this block, and this block must appear
#    BEFORE the existing 'if (READ_ONCE(mt7925_test_mt7927_sublink_roc) &&'
#    branch in the completion body.
if ! rg -Fq 'return 0;' <<< "$postkey_block"; then
	fail "postkey block does not return early"
fi
postkey_line=$(grep -Fn 'mt7925_test_mt7927_sublink_postkey' <<< "$completion_body" | head -1 | cut -d: -f1 || true)
sublink_branch_line=$(grep -Fn 'if (READ_ONCE(mt7925_test_mt7927_sublink_roc) && is_mt7927' <<< "$completion_body" | head -1 | cut -d: -f1 || true)
[[ -n $postkey_line ]] || fail "could not locate the postkey gate line"
[[ -n $sublink_branch_line ]] || fail "could not locate the existing sublink experimental branch"
if (( postkey_line > sublink_branch_line )); then
	fail "postkey gate (line $postkey_line) appears after the existing sublink ROC branch (line $sublink_branch_line) -- must intercept before either ROC path"
fi

printf 'postkey pending-state contract passed\n'
