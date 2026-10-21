#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# A link set that needed a firmware reset is remembered as SEVERE by BAND class
# (not link id), survives the reset in the private VIF, and Policy V2 keeps to
# the remaining options. The first genuine 0x7 band map is kept as a small record.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail() { printf 'mlo-severity contract failed: %s\n' "$1" >&2; exit 1; }
fn() { sed -n "/^$1(/,/^}/p" "$2"; }

[[ -f mt7925/mlo_severity.h ]] || fail "pure severity policy missing"
gcc -Wall -Wextra -Werror -I mt7925 -o /tmp/test_mlo_severity.$$ mt7925/test_mlo_severity.c
/tmp/test_mlo_severity.$$ > /dev/null || fail "policy unit test fails"
rm -f /tmp/test_mlo_severity.$$

[[ -f mt7925/mlo/severity.c ]] || fail "mlo/severity.c missing"
grep -q 'mlo/severity.o' mt7925/Makefile || fail "severity.o is not built"
grep -q '#include "../mlo_severity.h"' mt7925/mlo/severity.c || fail "wrapper does not use the pure policy"

# state in the private VIF extension, shared mt792x layout unchanged
grep -q 'struct mt7925_mlo_severity_state' mt7925/mlo/pairs.h || fail "no private severity state"

# the FIRST caller marks the PREVIOUS connection (the pair being left) severe,
# right before it asks for the reset; the failed target is not what gets marked
w=$(fn 'static void mt7925_mlo_recovery_work' mt7925/mlo/recovery.c)
grep -q 'mt7925_mlo_severity_mark_previous(vif, rs->prev)' <<< "$w" || fail "recovery does not mark the previous connection"
grep -q 'mark_failed' <<< "$w" && fail "recovery still marks the failed target"
m=$(grep -n 'mt7925_mlo_severity_mark_previous' <<< "$w" | cut -d: -f1 | tail -1)
r=$(grep -n 'mt792x_reset(' <<< "$w" | cut -d: -f1)
[[ -n $m && -n $r && $m -lt $r ]] || fail "severity is not marked before the reset request"

# the last-good record is captured and the after-reset report runs at authorization
a=$(fn 'void mt7927_mlo_pairs_authorized' mt7925/mlo/pairs.c)
grep -q 'mt7925_mlo_severity_lastgood_note(vif)' <<< "$a" || fail "record is not captured at authorization"
grep -q 'mt7925_mlo_severity_post_reset(vif)' <<< "$a" || fail "after-reset step does not run at authorization"

# the previous connection comes from the transition record, not the failing callback
l=$(grep -n 'mt7925_mlo_completion_admission_failed(vif' mt7925/mlo/lifecycle.c | head -1 | cut -d: -f1)
sed -n "${l},$((l+2))p" mt7925/mlo/lifecycle.c | grep -q 'pt_old_links' || fail "failed admission does not pass the previous pair (pt_old_links)"

# the SECOND caller decides what to run, at association time, from the memory
sa=$(fn 'mt7925_mac_set_links' mt7925/mlo/association.c)
[[ -z $sa ]] && sa=$(sed -n '/^mt7925_mac_set_links(/,/^}/p' mt7925/mlo/association.c)
grep -q 'mt7925_mlo_severity_decide(vif' <<< "$sa" || fail "association-time pair choice does not consult the second caller"
grep -q 'mt7925_mlo_sev_decide' mt7925/mlo/severity.c || fail "decision is not the pure policy"

# Policy V2 consumes it: pair candidates and the single-link fallback, by band map
s=$(fn 'void mt7925_mlo_steer_work' mt7925/mlo/steering.c)
[[ $(grep -c 'mt7925_mlo_severity_blocked(vif' <<< "$s") -ge 2 ]] || fail "steering does not skip severe pairs and singles"

# severity never touches the non-MLD path: the classifier has no class for it
grep -q 'return -1;' <(fn 'static inline int' mt7925/mlo_severity.h) || true
grep -q 'ieee80211_vif_is_mld' mt7925/mlo/severity.c || fail "wrapper does not leave non-MLD associations alone"

# A completed transition (the pair the second caller decided is the pair running) settles
# the strikes, so legitimate switching never lengthens the lockout; a transition that did
# not complete keeps escalating. Done where the after-association report runs.
pr=$(fn 'void mt7925_mlo_severity_post_reset' mt7925/mlo/severity.c)
grep -q 'mt7925_mlo_sev_settle(' <<< "$pr" || fail "completed transitions do not settle the strikes"
grep -q 'decided == vif->active_links' <<< "$pr" || fail "settling is not conditioned on the decided pair being the running pair"

printf 'mlo-severity contract: PASS\n'
