#!/usr/bin/env bash
# Contract for the Sep-18 replay test gate (mlo_sep18_replay): off by default,
# forces the legacy full two-TLV JOIN, recovers the survivor with the existing
# peer lifecycles only (no DEV/BSS/BMC context stage, no second JOIN shape),
# and never tears down the association.
set -u
cd "$(dirname "$0")/../.." || exit 1
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

src=mt7925/mlo/lifecycle.c

grep -Eq '^static bool mt7925_mlo_sep18_replay;' $src ||
	fail "mlo_sep18_replay must default to N (no initializer)"
grep -q 'module_param_named(mlo_sep18_replay' $src ||
	fail "mlo_sep18_replay module parameter is missing"
# The exclusion now encloses planning, including ALREADY_HELD as well as
# DELTA_ACQUIRE; replay must reach the legacy full JOIN for either plan.
dispatch=$(sed -n '/if (!mt7925_mlo_sep18_replay && is_mt7927/,/^\t\tif (!skip_legacy_join/p' $src)
grep -q 'mt7925_mlo_pt_plan(' <<< "$dispatch" &&
grep -q 'pt.kind == MT7925_MLO_PT_DELTA_ACQUIRE' <<< "$dispatch" ||
	fail "replay does not bypass the delta acquisition"

h=$(sed -n '/^static int mt7927_mlo_sep18_recover_survivor(/,/^}/p' $src)
[ -n "$h" ] || fail "sep18 survivor recovery function is missing"
grep -q 'mt7927_mlo_recreate_authorize_peer' <<< "$h" &&
grep -q 'mt7927_mlo_rebuild_peer_assoc_order' <<< "$h" ||
	fail "recovery must reuse the existing peer lifecycles"
grep -q 'mt7925_mlo_pt_peer_recover_next' <<< "$h" ||
	fail "recovery must use the pure peer-only ladder"
if grep -q 'mt7927_mlo_pt_reprogram_survivor_context\|mt76_connac_mcu_uni_add_dev\|mt7925_mcu_add_bss_info\|mt7925_mcu_set_mlo_roc\|mt7925_set_mlo_roc\|delta_acquire' <<< "$h"; then
	fail "replay recovery must not add a context stage or another JOIN shape"
fi
grep -q 'mlo_valid_rx_packets' <<< "$(sed -n '/^static bool mt7927_mlo_surv_poll_valid_rx(/,/^}/p' $src)" ||
	fail "survivor recovery is not judged on security-valid RX"
if grep -Eq 'ieee80211_connection_loss|ieee80211_disconnect|cfg80211_disconnected|ieee80211_stop_|wpa_' <<< "$h"; then
	fail "replay must not tear down or reassociate"
fi
printf 'sep18 replay contract passed\n'
