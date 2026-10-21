#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Make-before-break continuity contract: the security-valid survivor grant
# (continuity grant) is marked before anything is released, every ROC abort
# path that can hit a held MLO slot goes through the continuity guard, a
# failed replacement preparation fails safe with no destructive fallback,
# and destructive behaviour is reachable only behind default-off test gates.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'make-before-break contract failed: %s\n' "$1" >&2
	exit 1
}

main=mt7925/main.c
radio=mt7925/mlo/radio.c
mlo=mt7925/mlo/lifecycle.c

fn() { sed -n "/^$1(/,/^}/p" "$2"; }

# the guard is built on the pure invariant
g=$(fn 'static bool __mt7925_mlo_roc_abort_guard' $radio)
[[ -n $g ]] || fail "continuity guard is missing"
grep -q 'mt7925_mlo_roc_abort_check(' <<< "$g" || fail "guard does not use the pure invariant"
grep -q 'mt7927_mlo_destructive_test_gate()' <<< "$g" || fail "guard ignores the test gate"
grep -q 'MLO_CONTINUITY_ABORT_BLOCKED' <<< "$g" || fail "refused abort is not logged"
for f in token link_id bss_idx active_links valid_links reason; do
	grep -q "MLO_CONTINUITY_ABORT_BLOCKED:.*$f=" <<< "$g" ||
		fail "blocked log lacks $f"
done

# held-slot release filters through the guard BEFORE forgetting the slot
r=$(fn 'static void mt7925_mlo_roc_release_held' $radio)
gl=$(grep -n '__mt7925_mlo_roc_abort_guard(' <<< "$r" | head -1 | cut -d: -f1)
sl=$(grep -n 'mt7925_mlo_roc_state_release(' <<< "$r" | head -1 | cut -d: -f1)
[[ -n $gl && -n $sl && $gl -lt $sl ]] ||
	fail "held release forgets slots before asking the guard"

# every other abort sender is guarded
grep -q 'mt7925_mlo_roc_abort_guard(phy, mvif, phy->roc_token_id' <<< "$(fn 'static void mt7925_roc_iter' $main)" ||
	fail "roc_iter aborts unguarded"
grep -q 'mt7927_mlo_radio_expire(phy, vif)' <<< "$(fn 'static void mt7925_roc_iter' $main)" ||
	fail "roc_iter does not delegate its MLO expiry path"
[[ $(grep -c 'mt7925_mlo_roc_abort_guard(' <<< "$(fn 'static void mt7925_roc_iter' $main)") -ge 1 &&
   $(grep -c 'mt7925_mlo_roc_abort_guard(' <<< "$(fn 'bool mt7927_mlo_radio_expire' $radio)") -ge 1 ]] ||
	fail "roc_iter has an unguarded branch"
grep -q 'mt7925_mlo_roc_abort_guard(' <<< "$(fn 'int mt7925_abort_roc_token' $radio)" ||
	fail "abort_roc_token unguarded"
grep -q 'mt7925_mlo_roc_guard_links(' <<< "$(fn 'static void mt7925_mlo_roc_release_orphans' $radio)" ||
	fail "orphan release unguarded"
grep -q 'mt7925_mlo_roc_guard_links(' <<< "$(fn 'int mt7925_set_mlo_roc' $radio)" ||
	fail "JOIN timeout abort unguarded"
d=$(fn 'int mt7925_mlo_roc_delta_acquire' $radio)
[[ $(grep -c 'mt7925_mlo_roc_abort_guard(' <<< "$d") -ge 3 ]] ||
	fail "delta acquire has unguarded aborts"
[[ $(grep -c 'mt7925_mcu_abort_mlo_roc(' <<< "$d") -eq $(grep -c 'mt7925_mlo_roc_abort_guard(' <<< "$d") ]] ||
	fail "delta acquire abort count != guard count"

# continuity is marked before the shrink releases anything
c=$(fn 'int mt7927_mlo_change_vif_links' $mlo)
ml=$(grep -n 'mt7927_mlo_mark_continuity(dev, phy, vif, mvif,' <<< "$c" | head -1 | cut -d: -f1)
rl=$(grep -n 'mt7925_mlo_roc_release_transition(' <<< "$c" | head -1 | cut -d: -f1)
[[ -n $ml && -n $rl && $ml -lt $rl ]] ||
	fail "continuity grant is not marked before the shrink release"
grep -q 'MT7925_MLO_PT_PRESERVE_CONTINUITY_GRANT' <<< "$c" ||
	fail "transition never enters PRESERVE_CONTINUITY_GRANT"

# failed preparation: fail safe, no fallback
p=$(fn 'static int mt7927_mlo_pt_delta_acquire' $mlo)
grep -q 'MT7925_MLO_PT_PREPARE_SHADOW_GROUP' <<< "$p" || fail "no PREPARE_SHADOW_GROUP state"
grep -q 'PREPARE_BLOCKED_BY_HELD_GRANT' <<< "$p" || fail "blocked preparation not classified"
if grep -Eq 'mt7925_set_mlo_roc|release_held|abort_mlo_roc_all|release_transition|survivor_release' <<< "$p"; then
	fail "replacement preparation contains a destructive fallback"
fi

# handoff before release, only after validation
x=$(fn 'static void mt7927_mlo_pt_rx_ready' $mlo)
v1=$(grep -n 'MT7925_MLO_PT_VALIDATE_SHADOW_PATH' <<< "$x" | head -1 | cut -d: -f1)
v2=$(grep -n 'mt7925_mlo_continuity_handoff(' <<< "$x" | head -1 | cut -d: -f1)
v3=$(grep -n 'mt7925_mlo_roc_release_obsolete(' <<< "$x" | head -1 | cut -d: -f1)
[[ -n $v1 && -n $v2 && -n $v3 && $v1 -lt $v2 && $v2 -lt $v3 ]] ||
	fail "old grant can be released before validation + handoff"

# destructive paths are default-off test gates only
for p in mt7925_mlo_sep18_replay mt7925_mlo_security_rebind \
	 mt7925_mlo_regrant_parent_rebuild mt7925_mlo_survivor_rejoin; do
	grep -Eq "^static bool $p;" $mlo || fail "$p must default to N"
done
grep -q '^u8 mt7925_mlo_roc_release = 3;' mt7925/init.c ||
	fail "mlo_roc_release default must stay 3 (no release-all bit)"
grep -q 'mt7925_mlo_pt_destructive_gate(' <<< "$(fn 'bool mt7927_mlo_destructive_test_gate' $mlo)" ||
	fail "destructive gate is not the pure helper"
grep -q 'mt7925_mlo_pt_survivor_handoff_wanted(pt.kind,' <<< "$c" ||
	fail "survivor release is not gated"

printf 'make-before-break contract passed\n'
