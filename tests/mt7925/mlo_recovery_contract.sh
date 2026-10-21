#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# After a failed replacement admission the MLO subsystem watches the survivor
# and, only with TX-without-RX evidence, requests one firmware reset. The
# wrapper lives in mt7925/mlo/recovery.c; the decision is the pure policy.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail() { printf 'mlo-recovery contract failed: %s\n' "$1" >&2; exit 1; }
fn() { sed -n "/^$1(/,/^}/p" "$2"; }

[[ -f mt7925/mlo_recovery_policy.h ]] || fail "pure recovery policy missing"
gcc -Wall -Wextra -Werror -I mt7925 -o /tmp/test_mlo_recovery_policy.$$ \
	mt7925/test_mlo_recovery_policy.c
/tmp/test_mlo_recovery_policy.$$ > /dev/null || fail "policy unit test fails"
rm -f /tmp/test_mlo_recovery_policy.$$

[[ -f mt7925/mlo/recovery.c ]] || fail "mlo/recovery.c missing"
grep -q 'mlo/recovery.o' mt7925/Makefile || fail "recovery.o is not built"
grep -q '#include "../mlo_recovery_policy.h"' mt7925/mlo/recovery.c || fail "wrapper does not use the pure policy"

# the reset request is owned by the wrapper alone
for f in mt7925/mlo/*.c; do
	[[ $f == mt7925/mlo/recovery.c ]] && continue
	grep -q 'mt792x_reset(' "$f" && fail "$f requests a firmware reset outside the recovery wrapper"
done
grep -q 'mt792x_reset(&dev->mt76)' mt7925/mlo/recovery.c || fail "wrapper never requests the reset"

# state lives in the private VIF extension, shared mt792x layout unchanged
grep -q 'struct mt7925_mlo_recovery_state' mt7925/mlo/pairs.h || fail "no private recovery state"

# armed only after a restored survivor, never from the failure callback itself
r=$(fn 'static int mt7925_mlo_admit_repair' mt7925/mlo/completion.c)
grep -q 'mt7925_mlo_recovery_arm_survivor(vif' <<< "$r" || fail "repair does not arm the watch"
grep -q 'if (!ret)' <<< "$r" || grep -q 'ret == 0' <<< "$r" || fail "watch is armed even when the repair failed"

# teardown cancels the watch before the vif goes away
s=$(fn 'void mt7925_mlo_completion_stop' mt7925/mlo/completion.c)
grep -q 'mt7925_mlo_recovery_stop(vif)' <<< "$s" || fail "teardown does not cancel the recovery watch"

# the worker never takes the wiphy lock (teardown joins it while holding wiphy)
w=$(fn 'static void mt7925_mlo_recovery_work' mt7925/mlo/recovery.c)
[[ -n $w ]] || fail "no recovery worker"
grep -q 'wiphy_lock' <<< "$w" && fail "recovery worker takes the wiphy lock"
grep -q 'mt7925_mlo_rec_epoch(' <<< "$w" || fail "worker does not bind the watch to the association epoch"
grep -q 'c->assoc_epoch' <(fn 'static u64 mt7925_mlo_rec_epoch' mt7925/mlo/recovery.c) || fail "epoch is not the completion owner's association epoch"

# The reset leaves cleanly first: a deauth to the AP, so the AP drops its state for the
# link addresses before the reassociation that follows the reset. Order: previous pair
# marked, clean leave, reset. The leave is bounded, skipped when the association is
# already gone, and never takes the wiphy lock.
l=$(fn 'static void mt7925_mlo_recovery_clean_leave' mt7925/mlo/recovery.c)
[[ -n $l ]] || fail "no clean leave before the reset"
grep -q 'ieee80211_disconnect(vif' <<< "$l" || fail "clean leave does not ask mac80211 for the deauth"
grep -q 'cfg.assoc' <<< "$l" || fail "clean leave does not look at the association state"
grep -q 'time_before' <<< "$l" || fail "clean leave wait is not bounded"
grep -q 'wiphy_lock' <<< "$l" && fail "clean leave takes the wiphy lock"
a=$(grep -n 'mt7925_mlo_severity_mark_previous(vif' <<< "$w" | cut -d: -f1 | tail -1)
b=$(grep -n 'mt7925_mlo_recovery_clean_leave(vif)' <<< "$w" | cut -d: -f1 | tail -1)
c=$(grep -n 'mt792x_reset(&dev->mt76)' <<< "$w" | cut -d: -f1)
[[ -n $a && -n $b && -n $c ]] || fail "reset path misses the mark, the clean leave or the reset"
(( a < b && b < c )) || fail "order must be: mark previous, clean leave, reset"

# A grant-stage failure with a healthy radio (the delta ACQUIRE gets no firmware
# response) leaves the survivor alone with nothing to re-admit the dropped link.
# Hardware: no grant for either band next to a held survivor; the firmware grants
# only through the JOIN built at association time. The failsafe therefore asks the
# recovery wrapper for ONE reassociation (no firmware reset): previous pair marked,
# clean leave, the second caller decides the pair at the new association.
d=$(fn 'static int mt7927_mlo_pt_delta_acquire' mt7925/mlo/lifecycle.c)
[[ -z $d ]] && d=$(sed -n '/mt7925_mlo_roc_delta_acquire(phy, sub_mconf/,/^}/p' mt7925/mlo/lifecycle.c)
grep -q 'mt7925_mlo_recovery_request_reassoc(vif' <<< "$d" || fail "grant-stage failsafe does not ask for the reassociation"
grep -q 'pt_old_links' <<< "$d" || fail "reassociation request does not carry the previous pair"
q=$(fn 'void mt7925_mlo_recovery_request_reassoc' mt7925/mlo/recovery.c)
[[ -n $q ]] || fail "no reassociation request in the wrapper"
grep -q 'mt7925_mlo_reassoc_allowed(' <<< "$q" || fail "reassociation request is not rate limited by the pure policy"
grep -q 'wiphy_lock' <<< "$q" && fail "reassociation request takes the wiphy lock"
grep -q 'mt792x_reset' <<< "$q" && fail "reassociation request resets the firmware"
ra=$(sed -n '/if (rs->reassoc_req)/,/^	}/p' <<< "$w")
[[ -n $ra ]] || fail "worker has no reassociation branch"
grep -q 'mt792x_reset' <<< "$ra" && fail "reassociation branch resets the firmware"
a2=$(grep -n 'mt7925_mlo_severity_mark_previous(vif' <<< "$ra" | cut -d: -f1)
b2=$(grep -n 'mt7925_mlo_recovery_clean_leave(vif)' <<< "$ra" | cut -d: -f1)
[[ -n $a2 && -n $b2 ]] && (( a2 < b2 )) || fail "reassociation branch order must be: mark previous, clean leave"

# Policy V2's validator must not roll back into a pair that cannot be granted once the
# reassociation was requested for this association: the rollback is another add-link
# request nobody answers, and it races the reassociation (hardware: delta ACQUIRE gets
# no firmware response next to a held survivor). The pending state is bound to the
# association epoch, so it clears itself when the new association starts.
rp=$(fn 'bool mt7925_mlo_recovery_reassoc_pending' mt7925/mlo/recovery.c)
[[ -n $rp ]] || fail "no reassociation-pending query in the wrapper"
grep -q 'mt7925_mlo_rec_epoch(' <<< "$rp" || fail "reassociation-pending is not bound to the association epoch"
grep -q 'wiphy_lock' <<< "$rp" && fail "reassociation-pending takes the wiphy lock"
sv=$(fn 'static void mt7925_mlo_steer_switch_validate' mt7925/mlo/steering.c)
n=$(grep -c 'mt7925_mlo_recovery_reassoc_pending(vif)' <<< "$sv" || true)
(( n >= 2 )) || fail "validator does not skip the rollback in both failure branches (found $n)"
grep -q 'mt7925_mlo_recovery_reassoc_pending' mt7925/mlo/pairs.h || fail "query is not declared"

printf 'mlo-recovery contract: PASS\n'
