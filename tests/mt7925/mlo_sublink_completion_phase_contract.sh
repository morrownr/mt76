#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# The experimental standalone-SUB_LINK branch inside the 1->2 completion
# phase must resolve the resident survivor and dormant substitute exactly
# like the default path, must never touch the survivor's own resident
# state (no abort/deactivate/reassign/refresh against it), must request
# ROC for the substitute alone, and must keep the substitute invalid until
# a real, token-matched, status-checked grant -- then reuse the same
# admission-generation gating the default path already uses.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'sublink completion-phase contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
completion_body=$(awk '/^\tif \(hweight16\(old_links\) == 1 && hweight16\(new_links\) == 2/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "$body")
[[ -n $completion_body ]] || fail "could not isolate completion-phase branch body"

# Anchor on the exact single-line condition of the original sublink-only
# branch, not the (textually similar, but distinct and earlier) postkey
# short-circuit's multi-line condition -- both contain the substring
# READ_ONCE(mt7925_test_mt7927_sublink_roc).
sublink_block=$(awk '/if \(READ_ONCE\(mt7925_test_mt7927_sublink_roc\) && is_mt7927\(&dev->mt76\)\) \{/{flag=1} flag{print} flag && /^\t\t\}/{exit}' <<< "$completion_body")
[[ -n $sublink_block ]] || fail "could not isolate the experimental sublink branch"

# 1. Must send the standalone sublink request for the substitute (sub_id),
#    not the anchor.
if ! rg -Fq 'mt7925_set_sublink_roc(phy,' <<< "$sublink_block"; then
	fail "experimental branch does not call mt7925_set_sublink_roc()"
fi

# 2. Must not touch the anchor/survivor's resident state in this branch --
#    no abort/deactivate/reassign/refresh calls against it here (those
#    already ran, if at all, earlier/elsewhere in the shared setup code
#    that both paths share -- this check is scoped to the sublink_block
#    itself, which must contain none of them).
for forbidden in mt7927_mlo_note_bss mt7927_mlo_deactivate_wcid mt7927_mlo_reassign_band mt7927_mlo_refresh_reassigned_sta mt7925_abort_mlo_roc_all; do
	if rg -Fq "$forbidden" <<< "$sublink_block"; then
		fail "experimental branch calls $forbidden -- must not touch survivor state in this branch"
	fi
done

# 3. real_grant must be derived from the sublink call's own result, not
#    assumed true merely because the branch was taken.
if ! rg -q 'real_grant = ' <<< "$sublink_block"; then
	fail "experimental branch does not set real_grant from the sublink ROC result"
fi

# 4. Must not unconditionally force err = 0 in a way that also flips
#    real_grant true -- i.e. real_grant's assignment must be tied to the
#    sublink call's return value, not a bare 'true' literal.
if rg -Fq 'real_grant = true;' <<< "$sublink_block"; then
	fail "experimental branch hardcodes real_grant = true instead of deriving it from the sublink ROC call result"
fi

# 5. The shared post-branch logic (real_grant gate, roc_admitted/
#    admitted_generation stamping, traffic_active_links commit) must still
#    run for both the default and experimental paths -- i.e. the
#    experimental branch must not itself return early bypassing that
#    shared commit logic, so admission-generation gating stays identical
#    either way.
sublink_block_line_count=$(wc -l <<< "$sublink_block")
if rg -Fq 'return 0;' <<< "$sublink_block"; then
	fail "experimental branch returns early instead of falling through to the shared real_grant/commit logic used by the default path"
fi

printf 'sublink completion-phase contract passed\n'
