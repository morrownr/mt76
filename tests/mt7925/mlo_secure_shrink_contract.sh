#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Sole-secure-link preservation contract (2026-10-04 finding): a host-requested
# active-link transition that would release the only security-valid link must
# be refused BEFORE it is requested (mac80211 cannot be vetoed once its shrink
# starts); a rollback is confirmed only with security-valid RX after the
# actual outer completion. Stale descriptor-generation attribution is a separate
# integration gap; these source patterns alone cannot prove it.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail() { printf 'secure-shrink contract failed: %s\n' "$1" >&2; exit 1; }
fn() { sed -n "/^$1(/,/^}/p" "$2"; }

main=mt7925/mlo/steering.c
mlo=mt7925/mlo/lifecycle.c

[[ -f mt7925/mlo_secure_shrink.h ]] || fail "pure helper missing"
gcc -Wall -Wextra -Werror -I mt7925 -o /tmp/test_mlo_secure_shrink.$$ \
	mt7925/test_mlo_secure_shrink.c
/tmp/test_mlo_secure_shrink.$$ > /dev/null || fail "unit test fails"
rm -f /tmp/test_mlo_secure_shrink.$$

grep -q '#include "../mlo_secure_shrink.h"' $main || fail "controller does not use the helper"

# security evidence is built from valid-datapath RX of the current incarnation
s=$(fn 'static u16 mt7925_mlo_secure_links' $main)
[[ -n $s ]] || fail "secure link evaluation missing"
grep -q 'mlo_valid_rx_jiffies' <<< "$s" || fail "evidence is not security-valid RX"
grep -q 'wcid.link_valid' <<< "$s" || fail "evidence ignores link validity"
grep -q 'mt7925_mlo_pt_secure_rx_fresh' <<< "$s" || fail "evidence has no freshness bound"

# the request choke point is gated, with an explicit destructive-test bypass
g=$(fn 'static int mt7925_mlo_secure_gate' $main)
[[ -n $g ]] || fail "request gate missing"
grep -q 'mt7925_mlo_secure_transition_verdict(' <<< "$g" || fail "gate does not use the verdict"
grep -q 'mt7927_mlo_destructive_test_gate()' <<< "$g" || fail "gate has no explicit test bypass"
grep -q 'MLO_CONTINUITY_TRANSITION_VETOED' <<< "$g" || fail "veto is not logged"
n=$(fn 'int mt7925_mlo_request_active_links_negotiated' $main)
grep -q 'mt7925_mlo_secure_gate(' <<< "$n" || fail "negotiated request path is ungated"

# the selector never picks a vetoed pair and does not validate a refused request
w=$(fn 'void mt7925_mlo_steer_work' $main)
grep -q 'mt7925_mlo_secure_transition_verdict(' <<< "$w" || fail "selector ignores the verdict"
grep -q 'MLO_STEER_VETO' <<< "$w" || fail "selector veto is not logged"
grep -q 'MLO_STEER_REQUEST_REFUSED' <<< "$w" || fail "refused request still validated"

# rollback confirmation requires restored authenticated traffic
v=$(fn 'static void mt7925_mlo_steer_switch_validate' $main)
grep -q 'mt7925_mlo_completion_result(' <<< "$v" || fail "outer completion is not observed"
grep -q 'core.finished_jiffies, true' <<< "$v" || fail "RX is not newer than outer completion"
grep -q 'restored == prev_pair' <<< "$v" || fail "rollback does not require all restored members"
grep -q 'mt7925_mlo_completion_validate(vif, &mvif->mlo_steer_ticket, restored)' <<< "$v" || fail "rollback ticket is not validated"
grep -q 'MLO_STEER_ROLLBACK_UNVERIFIED' <<< "$v" || fail "unproven rollback is not reported"
c=$(grep -n 'MLO_STEER_ROLLBACK_CONFIRMED' <<< "$v" | head -1 | cut -d: -f1)
r=$(grep -Fn 'mt7925_mlo_completion_validate(vif, &mvif->mlo_steer_ticket, restored)' <<< "$v" | head -1 | cut -d: -f1)
[[ -n $c && -n $r && $r -lt $c ]] || fail "CONFIRMED is logged before restoration is proven"

# security evidence is reset when a link is (re)admitted or migrated
a=$(fn 'int mt7927_mlo_change_sta_links' $mlo)
grep -q 'WRITE_ONCE(mlink->mlo_valid_rx_jiffies, 0)' <<< "$a" ||
	fail "fresh admission inherits earlier security-RX timestamp"
grep -q 'WRITE_ONCE(peer->mlo_valid_rx_jiffies, 0)' $mlo ||
	fail "migration inherits earlier-radio security-RX timestamp"

printf 'secure-shrink contract: PASS\n'
