#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# A VIF-only worker has no lifetime-safe way to rediscover the peer STA
# (ieee80211_find_sta() is RCU-only-valid; mt792x_vif caches no peer
# pointer). This contract pins the STA-owned redesign: the two-stage
# work items and the pending-operation state they act on live on
# struct mt792x_sta, initialized exactly once per peer-STA lifetime,
# never embedded only in the VIF.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'STA-owned work contract failed: %s\n' "$1" >&2
	exit 1
}

sta_struct=$(awk '/^struct mt792x_sta \{/{flag=1} flag{print} flag && /^};/{exit}' mt792x.h)
[[ -n $sta_struct ]] || fail "struct mt792x_sta not found"

if ! rg -Fq 'struct wiphy_work postkey_stage1;' <<< "$sta_struct"; then
	fail "struct mt792x_sta does not own a struct wiphy_work postkey_stage1"
fi
if ! rg -Fq 'struct work_struct postkey_stage2;' <<< "$sta_struct"; then
	fail "struct mt792x_sta does not own a struct work_struct postkey_stage2"
fi
for field in postkey_pending postkey_sublink_id postkey_survivor_id postkey_generation postkey_roc_attempted; do
	if ! rg -q "$field" <<< "$sta_struct"; then
		fail "struct mt792x_sta does not own pending field: $field"
	fi
done

# The old VIF-owned pending fields must be gone from mt7927_mlo_state.
vif_state_struct=$(awk '/^struct mt7927_mlo_state \{/{flag=1} flag{print} flag && /^};/{exit}' mt792x_mlo_state.h)
[[ -n $vif_state_struct ]] || fail "struct mt7927_mlo_state not found"
for field in pending_sublink_roc pending_sublink_id pending_survivor_id pending_generation postkey_roc_attempted; do
	if rg -q "$field" <<< "$vif_state_struct"; then
		fail "struct mt7927_mlo_state still owns $field -- pending state must live only on mt792x_sta, per 'no work item embedded only in the VIF'"
	fi
done

# Initialized exactly once, in peer-STA init (mt7925_mac_sta_add()), before
# either work item can be queued.
sta_add_body=$(awk '/^int mt7925_mac_sta_add\(struct mt76_dev/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/association.c)
[[ -n $sta_add_body ]] || fail "mt7925_mac_sta_add() not found"
if ! rg -Fq 'wiphy_work_init(&msta->postkey_stage1,' <<< "$sta_add_body"; then
	fail "mt7925_mac_sta_add() does not wiphy_work_init() postkey_stage1"
fi
if ! rg -Fq 'INIT_WORK(&msta->postkey_stage2,' <<< "$sta_add_body"; then
	fail "mt7925_mac_sta_add() does not INIT_WORK() postkey_stage2"
fi

# Init must happen exactly once per driver (no second init site elsewhere).
init1_count=$(rg -Fc 'wiphy_work_init(&msta->postkey_stage1,' mt7925/main.c mt7925/mlo/association.c mt7925/mlo/lifecycle.c | awk -F: '{s+=$2} END{print s+0}')
init2_count=$(rg -Fc 'INIT_WORK(&msta->postkey_stage2,' mt7925/main.c mt7925/mlo/association.c mt7925/mlo/lifecycle.c | awk -F: '{s+=$2} END{print s+0}')
if [[ $init1_count -ne 1 ]]; then
	fail "expected exactly one wiphy_work_init(&msta->postkey_stage1, ...) call site, found $init1_count"
fi
if [[ $init2_count -ne 1 ]]; then
	fail "expected exactly one INIT_WORK(&msta->postkey_stage2, ...) call site, found $init2_count"
fi

printf 'STA-owned work contract passed\n'
