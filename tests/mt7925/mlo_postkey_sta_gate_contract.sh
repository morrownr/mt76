#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mt7927_mlo_postkey_attempt_locked() must never fire ROC just because of
# which callback invoked it -- the real gate is explicit pending-state
# validity (now peer-owned, on msta) plus verified group-key readiness.
# Since stage2 only ever runs after the wiphy-work barrier (the full
# key_switch_links() loop has already returned), keys_seen==0 is now a
# real terminal signal, not a "too early, retry" race -- it consumes
# pending directly rather than waiting for a second invocation.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'postkey STA-gate contract failed: %s\n' "$1" >&2
	exit 1
}

fn_body=$(awk '/mt7927_mlo_postkey_attempt_locked\(struct mt792x_dev/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $fn_body ]] || fail "mt7927_mlo_postkey_attempt_locked() is missing"

# 1. Must never key on old_links/new_links equality as its gate.
if rg -q 'old_links == new_links' <<< "$fn_body"; then
	fail "postkey gate compares old_links == new_links -- must gate on explicit pending-state validity instead"
fi

# 2. Must bail immediately (no-op) if postkey_pending is not set -- this
#    is what makes an unrelated invocation harmless.
if ! rg -Fq 'if (!msta->postkey_pending)' <<< "$fn_body"; then
	fail "postkey gate does not bail out when msta->postkey_pending is false"
fi

# 3. Structural validity requires: not already attempted, generation
#    match, substitute link membership in new_links, and survivor
#    resident+valid (resolved from the peer msta -- see the dedicated
#    survivor-domain contract for that specific object check).
for cond in 'postkey_roc_attempted' 'postkey_generation != mvif->mt7927_mlo.transition_generation' 'new_links & BIT(pend_sub)' 'surv_mlink->wcid.link_valid'; do
	if ! rg -Fq "$cond" <<< "$fn_body"; then
		fail "postkey gate does not check: $cond"
	fi
done

# 4. Key readiness verified via the shared group-key rebind helper, and
#    the keys_seen==0 / roc-call ordering is preserved (checked in detail
#    by mlo_postkey_key_readiness_contract.sh); here we just confirm the
#    call exists and precedes the ROC send.
if ! rg -Fq 'mt7925_mlo_group_key_rebind(dev, vif, pend_sub, &key_result)' <<< "$fn_body"; then
	fail "postkey gate does not call the shared mt7925_mlo_group_key_rebind() helper"
fi
keys_seen_line=$(grep -Fn 'key_result.keys_seen == 0' <<< "$fn_body" | head -1 | cut -d: -f1 || true)
roc_call_line=$(grep -Fn 'mt7925_set_sublink_roc(' <<< "$fn_body" | head -1 | cut -d: -f1 || true)
[[ -n $keys_seen_line ]] || fail "postkey gate does not check key_result.keys_seen == 0"
[[ -n $roc_call_line ]] || fail "postkey gate never calls mt7925_set_sublink_roc()"
if (( keys_seen_line > roc_call_line )); then
	fail "keys_seen==0 check (line $keys_seen_line) happens after the ROC call (line $roc_call_line) -- must gate before it"
fi

# 5. Exactly one ROC attempt per pending record: postkey_roc_attempted is
#    set right before the ROC call (after both the keys_seen==0 and
#    key_err checks, since either of those now terminates the attempt
#    without ever reaching the ROC call at all), and pending is cleared
#    on every terminal path via the shared clear helper.
attempted_set_line=$(grep -Fn 'msta->postkey_roc_attempted = true' <<< "$fn_body" | head -1 | cut -d: -f1 || true)
[[ -n $attempted_set_line ]] || fail "postkey gate never sets msta->postkey_roc_attempted = true"
if (( attempted_set_line < keys_seen_line )); then
	fail "postkey_roc_attempted is set (line $attempted_set_line) before the keys_seen==0 check (line $keys_seen_line)"
fi
if (( attempted_set_line > roc_call_line )); then
	fail "postkey_roc_attempted is set (line $attempted_set_line) after the ROC call (line $roc_call_line) -- must be marked consumed before sending it, not after"
fi

clear_call_count=$(grep -Fc 'mt7927_mlo_pending_clear(msta)' <<< "$fn_body" || true)
if (( clear_call_count < 4 )); then
	fail "expected mt7927_mlo_pending_clear(msta) on at least 4 distinct exit paths, found $clear_call_count"
fi

printf 'postkey STA-gate contract passed\n'
