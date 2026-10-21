#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# On any failure path (structural invalidity, key error/unavailability,
# ROC rejection/timeout, or activation failure) the pending record is
# consumed exactly once, the substitute stays traffic-invalid, the
# survivor is never touched, and there is no fallback to the full-pair
# ROC path.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'failure behavior contract failed: %s\n' "$1" >&2
	exit 1
}

fn_body=$(awk '/mt7927_mlo_postkey_attempt_locked\(struct mt792x_dev/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $fn_body ]] || fail "mt7927_mlo_postkey_attempt_locked() is missing"

# Pending consumed on every terminal path: structural invalid, keys
# unavailable, key error, ROC failure, and end-of-function (activation
# success or failure both fall through to a single final clear).
clear_count=$(grep -Fc 'mt7927_mlo_pending_clear(msta)' <<< "$fn_body" || true)
if (( clear_count < 4 )); then
	fail "expected mt7927_mlo_pending_clear(msta) on at least 4 distinct exit paths (structural, keys-unavailable, key-error, ROC-failure/final), found $clear_count"
fi

# Never touches the survivor's own resident state, and never falls back
# to the full-pair ROC path.
for forbidden in mt7927_mlo_reassign_active_bands mt7925_abort_mlo_roc_all mt7927_mlo_deactivate_wcid mt7925_set_mlo_roc; do
	if rg -Fq "$forbidden" <<< "$fn_body"; then
		fail "postkey_attempt_locked() calls $forbidden -- must never touch survivor state or fall back to full-pair ROC"
	fi
done

# Activation failure keeps the substitute traffic-invalid: the
# 'act_err' failure path must not be followed by any of the commit
# actions (checked structurally by prepare_commit_contract's ordering;
# here we additionally require an explicit keep-dormant trace on that
# path).
if ! rg -Fq 'reason=activation_failed' <<< "$fn_body"; then
	fail "no explicit keep-dormant trace for the activation-failure path"
fi

printf 'failure behavior contract passed\n'
