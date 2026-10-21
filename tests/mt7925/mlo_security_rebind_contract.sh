#!/usr/bin/env bash
# Contract for the post-regrant security rebind (mlo_security_rebind): off by
# default, armed only by the Sep-18 replay's real full-JOIN grant, rebinds BOTH
# active links (survivor included) in the fixed order, invalidates peer and BMC
# key state first, builds STA_REC_MLD from the post-grant target set, requires
# security-valid RX on every link before CONVERGED, adds no new JOIN shape and
# never tears the association down.
set -u
cd "$(dirname "$0")/../.." || exit 1
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

src=mt7925/mlo/lifecycle.c

grep -Eq '^static bool mt7925_mlo_security_rebind;' $src ||
	fail "mlo_security_rebind must default to N (no initializer)"
grep -q 'module_param_named(mlo_security_rebind' $src ||
	fail "mlo_security_rebind module parameter is missing"
grep -Eq 'mt7925_mlo_sep18_replay && mt7925_mlo_security_rebind' $src ||
	fail "reincarnation must be armed only by the sep18 replay full-JOIN grant"

f=$(sed -n '/^static int mt7927_mlo_security_rebind_pair(/,/^}/p' $src)
[ -n "$f" ] || fail "security rebind function is missing"
for h in mt7925_mlo_pt_rebind_links mt7925_mlo_pt_rebind_next \
	 mt7925_mlo_pt_key_reprogram_needed mt7925_mlo_pt_converge_allowed; do
	grep -q "$h" <<< "$f" || fail "rebind must use pure helper $h"
done
grep -q 'bmc->wcid.hw_key_idx = 255' <<< "$f" &&
grep -q 'peer->wcid.hw_key_idx = 255' <<< "$f" ||
	fail "peer and BMC hw_key_idx must both be invalidated before reprogramming"
for c in mt7925_mcu_sta_update mt7925_mlo_ptk_rebind mt7925_mlo_group_key_rebind \
	 mt7925_mcu_sta_authorize; do
	grep -q "$c" <<< "$f" || fail "rebind must call $c"
done
# fixed order INVALIDATE -> STA_REC -> PTK -> GTK -> AUTHORIZE -> WAIT_RX
order=$(grep -n 'case MT7925_MLO_PT_REBIND_' <<< "$f" | sed 's/.*REBIND_\([A-Z_]*\):.*/\1/' | tr '\n' ' ')
[ "$order" = "INVALIDATE STA_REC PTK GTK AUTHORIZE " ] ||
	fail "rebind phase order wrong: '$order'"
grep -q 'mlo_valid_rx_packets' <<< "$f" ||
	fail "rebind must be judged on security-valid RX"
if grep -Eq 'mt7925_set_mlo_roc|mt7925_mcu_set_mlo_roc|delta_acquire|mt7925_mcu_set_roc|ieee80211_connection_loss|ieee80211_disconnect|cfg80211_disconnected|ieee80211_stop_' <<< "$f"; then
	fail "rebind must not add a JOIN shape or tear down the association"
fi
grep -Eq 'mt7927_mlo_security_rebind_pair\(dev, vif, sta, msta, new_links\)' $src ||
	fail "rebind is not hooked into mt7927_mlo_change_sta_links"
grep -q 'mt7925_mlo_pt_mld_links' mt7925/mlo/peer.c ||
	fail "STA_REC_MLD must be built from the post-grant target set"
printf 'security rebind contract passed\n'
