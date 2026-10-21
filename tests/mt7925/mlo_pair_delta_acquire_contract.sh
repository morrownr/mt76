#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Production pair transition wiring: the grow phase of
# mt7927_mlo_change_vif_links() plans on the delta between the held survivor
# and the desired pair and, for an independent physical slot, acquires ONLY
# the added link on a fresh token instead of re-requesting the survivor in a
# full JOIN (firmware queues that behind the held survivor slot forever). A
# failed acquisition must fail safe: never release the survivor's slot.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'pair delta-acquire contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
[[ -n $body ]] || fail "mt7927_mlo_change_vif_links is missing"

grep -q 'mt7925_mlo_pt_plan(' <<< "$body" ||
	fail "grow phase does not plan the transition on the survivor/added delta"
grep -q 'MT7925_MLO_PT_DELTA_ACQUIRE' <<< "$body" ||
	fail "grow phase has no delta-acquire branch"
grep -q 'MT7925_MLO_PT_HIGHBAND_SHARED_SLOT' <<< "$body" ||
	fail "same-slot (5<->6) addition has no safe MLSR-required stop"
grep -q 'skip_legacy_join' <<< "$body" ||
	fail "delta acquisition can fall through to the legacy replacement JOIN"

delta=$(sed -n '/^int mt7925_mlo_roc_delta_acquire(/,/^}/p' mt7925/mlo/radio.c)
[[ -n $delta ]] || fail "mt7925_mlo_roc_delta_acquire is missing"

# the survivor's slot is never named in the delta request, nor aborted here
grep -q 'mt7925_mcu_set_mlo_single_acquire(' <<< "$delta" ||
	fail "delta acquisition does not use the single-TLV acquire builder"
if grep -q 'mt7925_mlo_roc_release_held\|mt7925_abort_mlo_roc_all' <<< "$delta"; then
	fail "delta acquisition releases held (survivor) slots"
fi
grep -q 'mt7925_mlo_pt_grant_check(' <<< "$delta" ||
	fail "delta grant is not validated against token/bss/slot"
grep -q 'timedout_token = token' <<< "$delta" ||
	fail "a late grant of a timed-out delta request would not be recognised"

# releases go to the token that owns each slot
rel=$(sed -n '/^static void mt7925_mlo_roc_release_held(/,/^}/p' mt7925/mlo/radio.c)
grep -q 'mt7925_mlo_roc_state_owner_token' <<< "$rel" ||
	fail "held-slot release ignores per-link owner tokens"

# Execute the real single-TLV builder and shared radio policy, capturing the
# bytes at the MCU send boundary. Default JOIN ACQUIRE uses 0xfe regardless
# of the expected slot; MLSR uses 0xff and explicit test policies retain
# their diagnostic choices. This survives moving policy into shared helpers.
python3 tests/mt7925/test_mlo_radio_execution.py \
	RadioExecutionTest.test_delta_acquire_serializes_firmware_selected_band ||
	fail "delta ACQUIRE serialized request violates radio policy"

# the physical slot comes from one central helper
grep -q 'mt7925_mlo_band_slot' mt7925/mlo_pair_transition.h ||
	fail "central band->slot helper is missing"

printf 'pair delta-acquire contract passed\n'
