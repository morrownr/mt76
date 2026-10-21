#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# The pre-ROC group-key experiment must run strictly inside the existing
# standalone-sublink experimental branch, requiring BOTH
# test_mt7927_sublink_roc=1 and test_mt7927_sublink_prekey=1, so this
# isolates exactly one new variable (group-key presence before the ROC
# call) on top of the already-isolated sublink-ROC variable -- never
# combined with the full-pair path, and never on by itself.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'sublink prekey completion-phase contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
completion_body=$(awk '/^\tif \(hweight16\(old_links\) == 1 && hweight16\(new_links\) == 2/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "$body")
[[ -n $completion_body ]] || fail "could not isolate completion-phase branch body"

# Anchor on the exact single-line condition of the original sublink-only
# branch, not the (textually similar, earlier) postkey short-circuit's
# multi-line condition -- both contain the substring
# READ_ONCE(mt7925_test_mt7927_sublink_roc).
sublink_block=$(awk '/if \(READ_ONCE\(mt7925_test_mt7927_sublink_roc\) && is_mt7927\(&dev->mt76\)\) \{/{flag=1} flag{print} flag && /^\t\t\}/{exit}' <<< "$completion_body")
[[ -n $sublink_block ]] || fail "could not isolate the experimental sublink branch"

# 1. Prekey path must require BOTH flags together.
prekey_block=$(awk '/mt7925_test_mt7927_sublink_prekey/{flag=1} flag{print} flag && /^\t\t\t\}/{exit}' <<< "$sublink_block")
[[ -n $prekey_block ]] || fail "no prekey-gated block found inside the sublink experimental branch"
if ! rg -q 'READ_ONCE\(mt7925_test_mt7927_sublink_prekey\)' <<< "$sublink_block"; then
	fail "prekey path is not gated behind READ_ONCE(mt7925_test_mt7927_sublink_prekey)"
fi

# 2. Ordering: prekey rebind must run before the standalone ROC call
#    (mt7925_set_sublink_roc), not after.
prekey_call_line=$(grep -Fn 'mt7925_mlo_group_key_rebind(' <<< "$sublink_block" | head -1 | cut -d: -f1 || true)
roc_call_line=$(grep -Fn 'mt7925_set_sublink_roc(phy,' <<< "$sublink_block" | head -1 | cut -d: -f1 || true)
[[ -n $prekey_call_line ]] || fail "sublink branch never calls mt7925_mlo_group_key_rebind()"
[[ -n $roc_call_line ]] || fail "sublink branch does not call mt7925_set_sublink_roc()"
if (( prekey_call_line > roc_call_line )); then
	fail "prekey rebind (line $prekey_call_line) runs after the standalone ROC call (line $roc_call_line) -- must run before it"
fi

# 3. On prekey failure, ROC must not be issued at all, and the branch must
#    not fall through to a full-pair fallback -- it must reach the shared
#    dormancy handling (no goto to a full-pair path exists in this file,
#    so the only requirement here is that ROC is skipped).
if ! rg -q 'if \(prekey_err\)' <<< "$sublink_block"; then
	fail "sublink branch does not check a prekey_err result before proceeding"
fi
prekey_err_check_line=$(grep -Fn 'if (prekey_err)' <<< "$sublink_block" | head -1 | cut -d: -f1 || true)
[[ -n $prekey_err_check_line ]] || fail "could not locate the prekey_err check"
if (( prekey_err_check_line < prekey_call_line )); then
	fail "prekey_err is checked (line $prekey_err_check_line) before the rebind call that sets it (line $prekey_call_line)"
fi
if (( prekey_err_check_line > roc_call_line )); then
	fail "prekey_err is not checked before the standalone ROC call -- a failed prekey rebind would not prevent issuing ROC"
fi

# 4. No peer STA_REC creation and no traffic-eligibility flip inside the
#    prekey block itself.
for forbidden in mt7925_mac_sta_add_links mt7925_mlo_reconcile_link mt7925_mac_link_sta_assoc; do
	if rg -Fq "$forbidden" <<< "$prekey_block"; then
		fail "prekey block calls $forbidden -- must not create/activate the peer STA_REC"
	fi
done
if rg -Fq 'wcid.link_valid = true' <<< "$prekey_block"; then
	fail "prekey block marks a wcid traffic-eligible -- must stay dormant regardless of prekey outcome"
fi

printf 'sublink prekey completion-phase contract passed\n'
