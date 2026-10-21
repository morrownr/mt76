#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Make-before-break passivity contract: before a pair transition holds a
# security-valid continuity grant, the MBB machinery only records grant
# incarnations. It must not refuse aborts, change the ROC grant handling,
# or mark a continuity owner during ordinary association.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'mbb passivity contract failed: %s\n' "$1" >&2
	exit 1
}

main=mt7925/mlo/radio.c
mlo=mt7925/mlo/lifecycle.c
mcu=mt7925/mlo/radio.c
st=mt7925/mlo_roc_state.h

fn() { sed -n "/^$1(/,/^}/p" "$2"; }

# the only thing that can refuse an abort is a security-valid continuity owner
chk=$(fn 'mt7925_mlo_roc_abort_check' "$st")
[[ -n $chk ]] || fail "abort check missing"
grep -q 'continuity_owner && st->grant\[i\].secure_rx' <<< "$chk" ||
	fail "abort refusal not conditioned on a security-valid continuity owner"
[[ $(grep -c 'MT7925_MLO_ABORT_BLOCK_CONTINUITY;' <<< "$chk") == 1 ]] ||
	fail "abort check has more than one refusal path"

# a continuity owner is only created by continuity_mark with secure_rx
[[ $(grep -c 'continuity_owner = 1' "$st") == 2 ]] ||
	fail "unexpected writer of continuity_owner"
mk=$(fn 'mt7925_mlo_continuity_mark' "$st")
grep -q 'if (!secure_rx)' <<< "$mk" || fail "mark sets owner without secure RX"

# the kernel marks only a held slot, and only from the transition paths
m=$(fn 'static int mt7927_mlo_mark_continuity' "$mlo")
grep -q 'st->held_links & BIT(link_id)' <<< "$m" ||
	fail "continuity mark not restricted to a held slot"
callers=$(grep -n 'mt7927_mlo_mark_continuity(dev' "$mlo" | wc -l)
[[ $callers == 2 ]] || fail "unexpected continuity mark callers ($callers)"
grep -q 'if (rem && hweight16(new_links) == 1)' "$mlo" ||
	fail "shrink mark not gated on a single-survivor shrink"

# ROCEV grant handlers: MBB only adds bookkeeping inside the existing
# late-grant branch; no new early return and no new abort
event=$(fn 'bool mt7927_mlo_radio_event' "$mcu")
# GRANT and SUB_LINK now delegate to one owner; late-event bookkeeping
# is shared rather than duplicated in two transport handlers.
[[ $(grep -c 'mt7925_mlo_grant_note' <<< "$event") == 1 ]] ||
	fail "unexpected grant bookkeeping in the grant handlers"
[[ $(grep -c 'MT7925_MLO_GRANT_ORPHAN);' <<< "$event") == 1 ]] ||
	fail "grant handler records something other than late-grant orphans"
if grep -n 'mt7925_mlo_roc_abort_guard\|mt7925_mcu_abort' <<< "$event" |
	grep -q 'grant_note'; then
	fail "grant handler aborts from MBB bookkeeping"
fi

# guard is transparent when no owner exists: no MCU abort is dropped
g=$(fn 'static bool __mt7925_mlo_roc_abort_guard' "$main")
grep -q 'if (!mvif)' <<< "$g" || fail "guard without mvif fallthrough"
grep -q 'return true;' <<< "$g" || fail "guard never allows"

# the pure passivity unit test passes
gcc -Wall -Wextra -Werror -I mt7925 -o /tmp/test_mlo_mbb_passive.$$ \
	mt7925/test_mlo_mbb_passive.c
/tmp/test_mlo_mbb_passive.$$ > /dev/null || fail "passivity unit test fails"
rm -f /tmp/test_mlo_mbb_passive.$$

printf 'mbb passivity contract ok\n'
