#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# kfree_rcu() alone does not remove firmware STA_REC state: the existing
# mt7925_mac_sta_unwind_links_host() helper (used by
# mt7925_mac_sta_add_links()'s own error path) proves this -- it only does
# host-side wcid-table/RCU-slot cleanup, never sending
# MT76_STA_INFO_STATE_NONE to firmware. The real removal primitive is
# mt7925_mac_link_sta_remove(), which does send it. This contract pins:
# a newly-created peer whose prepare fails gets the REAL removal; a
# pre-existing resident peer is left alone on reconcile failure.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'rollback contract failed: %s\n' "$1" >&2
	exit 1
}

# 1. mt7925_mac_link_sta_remove() is exposed (non-static) for
#    cross-translation-unit use, with a prototype in mlo/decls.h.
if rg -Fq 'static void mt7925_mac_link_sta_remove(' mt7925/main.c; then
	fail "mt7925_mac_link_sta_remove() is still static -- mt7927_mlo.c cannot call it for real firmware rollback"
fi
if ! rg -Fq 'void mt7925_mac_link_sta_remove(struct mt76_dev' mt7925/mlo/decls.h; then
	fail "mt7925_mac_link_sta_remove() has no prototype in mlo/decls.h"
fi

prepare_body=$(awk '/^mt7927_mlo_postkey_prepare\(/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $prepare_body ]] || fail "mt7927_mlo_postkey_prepare() is missing"

# 2. On a newly-created peer (created==true) whose reconcile fails: the
#    real removal primitive runs, not just kfree_rcu().
fail_block=$(awk '/^fail:/{flag=1} flag{print} flag && /^}/{exit}' <<< "$prepare_body")
[[ -n $fail_block ]] || fail "could not isolate prepare's fail: rollback block"
if ! rg -Fq 'if (created)' <<< "$fail_block"; then
	fail "rollback does not branch on whether this prepare call actually created the peer"
fi
if ! rg -Fq 'mt7925_mac_link_sta_remove(' <<< "$fail_block"; then
	fail "rollback for a newly-created peer does not call the real mt7925_mac_link_sta_remove() -- firmware STA_REC would leak"
fi
if ! rg -Fq 'kfree_rcu(mlink' <<< "$fail_block"; then
	fail "rollback does not release the mlink object via the established kfree_rcu() lifetime mechanism"
fi
remove_line=$(grep -Fn 'mt7925_mac_link_sta_remove(' <<< "$fail_block" | head -1 | cut -d: -f1)
kfree_line=$(grep -Fn 'kfree_rcu(mlink' <<< "$fail_block" | head -1 | cut -d: -f1)
if (( kfree_line < remove_line )); then
	fail "mlink is kfree_rcu()'d (line $kfree_line) before the firmware removal command is sent (line $remove_line) -- use-after-free of the object mt7925_mac_link_sta_remove() needs"
fi

# 3. RCU slot is cleared as part of rollback.
if ! rg -Fq 'msta->link[pend_sub], NULL' <<< "$fail_block"; then
	fail "rollback does not clear the published msta->link[] RCU slot"
fi

# 4. A pre-existing resident peer (created==false) is left resident on
#    reconcile failure -- no removal call for that case. Verify the
#    removal/RCU-clear lines are inside the 'if (created)' guard, not
#    unconditional.
created_guard_line=$(grep -Fn 'if (created)' <<< "$fail_block" | head -1 | cut -d: -f1)
if (( remove_line < created_guard_line )); then
	fail "mt7925_mac_link_sta_remove() runs before the 'if (created)' guard -- would destroy a pre-existing resident peer on reconcile failure"
fi

printf 'rollback contract passed\n'
