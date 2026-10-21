#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# A replacement link that never reaches secure RX must not stay published.
# mac80211 ignores the change_sta_links error and keeps the requested mask, so
# the completion owner restores the survivor after the outer setter returns.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail() { printf 'mlo-failed-admission contract failed: %s\n' "$1" >&2; exit 1; }
fn() { sed -n "/^$1(/,/^}/p" "$2"; }

# the pending-RX helper reports the failed generation instead of returning void
grep -q '^static int mt7927_mlo_pt_rx_pending' mt7925/mlo/lifecycle.c ||
	fail "pending-RX helper does not return an error"
grep -q 'return -ENODATA;' <(fn 'static int mt7927_mlo_pt_rx_pending' mt7925/mlo/lifecycle.c) ||
	fail "failed replacement generation is not reported"

# the fresh-admission branch must notify the owner and leave through error cleanup
b=$(grep -n 'err = mt7927_mlo_pt_rx_pending' mt7925/mlo/lifecycle.c | cut -d: -f1)
[[ -n $b ]] || fail "fresh admission ignores the pending-RX result"
w=$(sed -n "${b},$((b+16))p" mt7925/mlo/lifecycle.c)
grep -q 'mt7925_mlo_completion_admission_failed(vif, old_links' <<< "$w" ||
	fail "failed admission does not notify the completion owner"
grep -q 'goto out_clear_adding' <<< "$w" || fail "failed admission is not unwound"
grep -q 'mlo_await_valid_rx, false' <<< "$w" || fail "failed link keeps the late-RX await open"

# the incoming-GTK switch of the same outer setter must not cancel the repair
k=$(sed -n '/^int mt7927_mlo_assoc_set_key/,/^}/p' mt7925/mlo/association.c)
grep -q 'mt7925_mlo_completion_key_changed(vif)' <<< "$k" || fail "MLD set_key cancels the pending repair"
grep -q 'mt7925_mlo_completion_changed(vif)' <<< "$k" && fail "MLD set_key uses the transition-cancelling hook"

# the owner restores through the real core setter, never by writing the mask
c=$(cat mt7925/mlo/completion.c)
grep -q 'ieee80211_set_active_links(vif, af.survivor)' <<< "$c" || fail "repair does not use the core setter"
grep -qE 'vif->active_links *= ' <<< "$c" && fail "completion owner writes active_links directly"
grep -q 'mt7925_mlo_admit_repair(vif)' <<< "$c" || fail "work function never repairs"

printf 'mlo-failed-admission contract: PASS\n'
