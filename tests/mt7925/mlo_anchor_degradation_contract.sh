#!/bin/sh
# SPDX-License-Identifier: ISC

set -eu

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

score_fn=$(sed -n '/^static s32 mt7925_mlo_pair_score_parts(/,/^}/p' mt7925/mlo/steering.c)
steer_fn=$(sed -n '/^void mt7925_mlo_steer_work(/,/^}/p' mt7925/mlo/steering.c)

grep -q '#define MT7925_MLO_STEER_6G_DEGRADE_THRESHOLD[[:space:]]*-75' \
	mt7925/mlo/steering.c || fail "missing -75 dBm degradation threshold"
grep -q '#define MT7925_MLO_STEER_6G_DEGRADE_PENALTY[[:space:]]*30' \
	mt7925/mlo/steering.c || fail "missing 30-point degradation penalty"
# Policy v2: any current pair that rides on the 6 GHz anchor (link 2) is
# degraded by it, and only the current pair carries the penalty.
printf '%s\n' "$score_fn" | grep -q '(current_links & BIT(2))' ||
	fail "degradation rule is not gated by the current pair riding on the anchor"
printf '%s\n' "$score_fn" | grep -q 'anchor_degraded && pair == current_links' ||
	fail "degradation penalty is not limited to the current pair"
# Dormant RSSI is unknown, not an estimate copied from the anchor. Such
# candidates may only enter the explicit degraded-pair exploration tier.
printf '%s\n' "$score_fn" | grep -q 'mstate == MT7925_MLO_METRIC_UNKNOWN' ||
	fail "unknown dormant metrics are not distinguished"
printf '%s\n' "$score_fn" | grep -q '\*is_experimental = true' ||
	fail "unknown dormant metrics are disguised as measured candidates"
printf '%s\n' "$steer_fn" | grep -q 'cur_degrade_penalty > 0 || !cur_is_mode' ||
	fail "unknown exploration is not gated by actual anchor degradation"
printf '%s\n' "$score_fn" | grep -q 'MT7925_MLO_STEER_6G_DEGRADE_PENALTY' ||
	fail "pair 0x6 does not apply the degradation penalty"
printf '%s\n' "$score_fn" | grep -q 'mlo_link2_rssi_override' ||
	fail "scorer does not consume the Link 2 RSSI override"
printf '%s\n' "$steer_fn" | grep -q 'anchor_rssi=' ||
	fail "steering trace does not expose anchor RSSI"
printf '%s\n' "$steer_fn" | grep -q 'degrade_penalty=' ||
	fail "steering trace does not expose the degradation penalty"
grep -q 'debugfs_create_file("mlo_link2_rssi_override"' mt7925/debugfs.c ||
	fail "Link 2 RSSI override is not registered in debugfs"

echo "PASS: anchor degradation steering contract is present"
