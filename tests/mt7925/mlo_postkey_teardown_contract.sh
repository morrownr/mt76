#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# MT76_STA_EVENT_DISASSOC (the ASSOC->AUTH sta_state step) fires strictly
# before the sta_info destroy sequence's later drv_sta_state(NOTEXIST) call
# and cleanup_single_sta()/sta_info_free() -- msta is guaranteed live here.
# It also fires with the wiphy mutex already held and dev->mt76.mutex NOT
# yet held by this function (mt7925_mac_sta_event() only takes that mutex
# internally, per-call, inside mt7925_mac_link_sta_assoc_locked() for the
# ASSOC case). So cancellation here is deadlock-free: wiphy_work_cancel()
# only needs the wiphy mutex (already held, and non-blocking regardless),
# and cancel_work_sync() on stage2 (which itself needs dev->mt76.mutex) can
# safely wait for it to finish, since we don't hold that mutex yet.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'teardown contract failed: %s\n' "$1" >&2
	exit 1
}

fn_body=$(awk '/^int mt7925_mac_sta_event\(/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/association.c)
[[ -n $fn_body ]] || fail "mt7925_mac_sta_event() is missing"

if ! rg -Fq 'MT76_STA_EVENT_DISASSOC' <<< "$fn_body"; then
	fail "mt7925_mac_sta_event() has no MT76_STA_EVENT_DISASSOC handling"
fi

disassoc_block=$(awk '/if \(ev == MT76_STA_EVENT_DISASSOC\)/{flag=1} flag{print} flag && /^\t\}/{exit}' <<< "$fn_body")
[[ -n $disassoc_block ]] || fail "could not isolate the DISASSOC branch"

if ! rg -Fq 'wiphy_work_cancel(' <<< "$disassoc_block"; then
	fail "DISASSOC branch does not call wiphy_work_cancel() for stage1"
fi
if ! rg -Fq '&msta->postkey_stage1' <<< "$disassoc_block"; then
	fail "DISASSOC branch does not cancel msta->postkey_stage1"
fi
if ! rg -Fq 'cancel_work_sync(&msta->postkey_stage2);' <<< "$disassoc_block"; then
	fail "DISASSOC branch does not cancel_work_sync() stage2"
fi
if ! rg -Fq 'mt7927_mlo_pending_clear(msta)' <<< "$disassoc_block"; then
	fail "DISASSOC branch does not clear pending state consistently via mt7927_mlo_pending_clear()"
fi

# Cancellation must occur before this function ever acquires
# dev->mt76.mutex (directly or via mt792x_mutex_acquire) -- the whole
# point of using the DISASSOC hook instead of MT76_STA_EVENT_ASSOC's own
# "_locked" pattern.
if rg -Fq 'mt792x_mutex_acquire' <<< "$disassoc_block"; then
	fail "DISASSOC branch acquires dev->mt76.mutex before/around cancellation -- this reintroduces the cancel-under-mutex deadlock risk"
fi

# The DISASSOC branch must appear before the pre-existing
# 'if (ev != MT76_STA_EVENT_ASSOC) return 0;' early-return, so it isn't
# shadowed.
disassoc_line=$(grep -Fn 'if (ev == MT76_STA_EVENT_DISASSOC)' <<< "$fn_body" | head -1 | cut -d: -f1)
assoc_gate_line=$(grep -Fn 'if (ev != MT76_STA_EVENT_ASSOC)' <<< "$fn_body" | head -1 | cut -d: -f1 || true)
[[ -n $assoc_gate_line ]] || fail "could not locate the pre-existing ASSOC-only early return"
if (( disassoc_line > assoc_gate_line )); then
	fail "DISASSOC branch (line $disassoc_line) appears after the ASSOC-only early return (line $assoc_gate_line) -- would never run"
fi

printf 'teardown contract passed\n'
