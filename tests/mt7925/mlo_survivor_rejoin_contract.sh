#!/usr/bin/env bash
# Contract for the survivor ownership handoff experiment (mlo_survivor_rejoin):
# gated off by default, survivor secure RX required before the incoming
# request, bounded existing recovery lifecycles only, no association teardown.
set -u
cd "$(dirname "$0")/../.." || exit 1
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

src=mt7925/mlo/lifecycle.c

grep -Eq '^static bool mt7925_mlo_survivor_rejoin;' $src ||
	fail "mlo_survivor_rejoin must default to N (no initializer)"
grep -q 'module_param_named(mlo_survivor_rejoin' $src ||
	fail "mlo_survivor_rejoin module parameter is missing"

h=$(sed -n '/^static int mt7927_mlo_pt_survivor_handoff(/,/^}/p' $src)
[ -n "$h" ] || fail "survivor handoff function is missing"

# never folds the incoming link into the survivor request
if grep -q 'mt7927_mlo_pt_delta_acquire\|mt7925_set_mlo_roc\|mt7925_mcu_set_mlo_roc' <<< "$h"; then
	fail "handoff must not request the incoming link or a full JOIN"
fi
# survivor-only release and survivor-only re-JOIN
grep -q 'mt7925_mlo_roc_survivor_release(' <<< "$h" ||
	fail "handoff does not release only the survivor slot"
grep -q 'mt7925_mlo_roc_delta_acquire(phy, smconf, BIT(sl)' <<< "$h" ||
	fail "survivor re-JOIN is not a single-link request for the survivor"
# secure RX is the terminal criterion, failure is FAILED_SAFE
grep -q 'survivor_rx_not_recovered' <<< "$h" ||
	fail "no fail-safe when survivor RX is not recovered"
grep -q 'mlo_valid_rx_packets' <<< "$(sed -n '/^static bool mt7927_mlo_surv_poll_valid_rx(/,/^}/p' $src)" ||
	fail "survivor recovery is not judged on security-valid RX"
# only existing bounded lifecycles
grep -q 'mt7927_mlo_recreate_authorize_peer' <<< "$h" &&
grep -q 'mt7927_mlo_rebuild_peer_assoc_order' <<< "$h" ||
	fail "recovery must reuse the existing peer lifecycles"
# no association teardown
if grep -Eq 'ieee80211_connection_loss|ieee80211_disconnect|cfg80211_disconnected|ieee80211_stop_|wpa_' <<< "$h"; then
	fail "handoff must not tear down or reassociate"
fi

# dispatch: gated, and the handoff runs strictly before the incoming acquire
disp=$(sed -n '/pt.kind == MT7925_MLO_PT_DELTA_ACQUIRE) {/,/skip_legacy_join = true;/p' $src)
grep -q 'mt7925_mlo_pt_survivor_handoff_wanted' <<< "$disp" ||
	fail "handoff is not gated by the plan/param helper"
hl=$(grep -n 'mt7927_mlo_pt_survivor_handoff(dev' <<< "$disp" | head -1 | cut -d: -f1)
al=$(grep -n 'mt7927_mlo_pt_delta_acquire(dev' <<< "$disp" | head -1 | cut -d: -f1)
[ -n "$hl" ] && [ -n "$al" ] && [ "$hl" -lt "$al" ] ||
	fail "survivor handoff must precede the incoming acquire"

# traces
for t in MLO_SURVIVOR_RELEASE MLO_SURVIVOR_REJOIN_REQ MLO_SURVIVOR_REJOIN_GRANT \
	 MLO_SURVIVOR_RECOVER_STAGE MLO_SURVIVOR_RX_READY \
	 MLO_PAIR_TRANSITION_TIMELINE; do
	grep -q "$t" $src || fail "trace $t is missing"
done

printf 'survivor rejoin contract passed\n'
