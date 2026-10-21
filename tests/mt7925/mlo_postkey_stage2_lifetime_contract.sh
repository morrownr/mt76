#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Stage 2 derives every object fresh from proven-resident, driver-owned
# state -- msta via container_of(), mvif via msta->vif, vif via the
# established container_of((void *)mvif, struct ieee80211_vif, drv_priv)
# pattern, and the peer struct ieee80211_sta * via wcid_to_sta() on the
# always-resident deflink wcid (never an RCU-only ieee80211_find_sta()
# pointer retained past its critical section). It acquires dev->mt76.mutex
# exactly once, around the locked postkey helper, and never holds an
# RCU-only pointer across the ROC wait inside that helper.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'stage-2 lifetime contract failed: %s\n' "$1" >&2
	exit 1
}

fn_body=$(awk '/^void mt7927_mlo_postkey_stage2_work\(/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $fn_body ]] || fail "mt7927_mlo_postkey_stage2_work() is missing"

if ! rg -Fq 'container_of(work, struct mt792x_sta,' <<< "$fn_body"; then
	fail "stage2 does not derive msta via container_of() from the work_struct"
fi
if ! rg -Fq 'postkey_stage2);' <<< "$fn_body"; then
	fail "stage2's container_of() does not name the postkey_stage2 member"
fi
if ! rg -Fq 'msta->vif' <<< "$fn_body"; then
	fail "stage2 does not derive mvif via msta->vif"
fi
if ! rg -Fq 'container_of((void *)mvif, struct ieee80211_vif, drv_priv)' <<< "$fn_body"; then
	fail "stage2 does not derive vif via the established container_of((void *)mvif, ...) pattern"
fi
if ! rg -Fq 'wcid_to_sta(&msta->deflink.wcid)' <<< "$fn_body"; then
	fail "stage2 does not derive the peer sta via wcid_to_sta(&msta->deflink.wcid)"
fi

# Rejects safely if any required object or the pending generation is
# invalid, before acquiring the mutex.
mutex_line=$(grep -Fn 'mt792x_mutex_acquire(dev);' <<< "$fn_body" | head -1 | cut -d: -f1 || true)
[[ -n $mutex_line ]] || fail "stage2 never acquires dev->mt76.mutex"
if ! rg -Fq 'if (!mvif)' <<< "$fn_body"; then
	fail "stage2 does not guard against a NULL mvif"
fi
if ! rg -Fq 'if (!sta' <<< "$fn_body"; then
	fail "stage2 does not guard against a NULL resolved peer sta"
fi
sta_guard_line=$(grep -Fn 'if (!sta' <<< "$fn_body" | head -1 | cut -d: -f1)
if (( sta_guard_line > mutex_line )); then
	fail "stage2's sta validity guard (line $sta_guard_line) runs after acquiring the mutex (line $mutex_line) -- must reject before locking"
fi

# Exactly one mutex acquire/release pair, wrapping the locked helper call.
acquire_count=$(grep -Fc 'mt792x_mutex_acquire(dev);' <<< "$fn_body" || true)
release_count=$(grep -Fc 'mt792x_mutex_release(dev);' <<< "$fn_body" || true)
if [[ $acquire_count -ne 1 || $release_count -ne 1 ]]; then
	fail "expected exactly one mutex acquire/release pair, found acquire=$acquire_count release=$release_count"
fi
if ! rg -Fq 'mt7927_mlo_postkey_attempt_locked(' <<< "$fn_body"; then
	fail "stage2 does not call mt7927_mlo_postkey_attempt_locked()"
fi

# No RCU section in stage2 at all -- the peer sta comes from wcid_to_sta(),
# not from a fresh ieee80211_find_sta() call, so there is nothing to keep
# an RCU-only pointer alive across the mutex/ROC wait.
if rg -Fq 'rcu_read_lock' <<< "$fn_body"; then
	fail "stage2 takes an RCU read lock -- it must not retain or reacquire an RCU-only pointer across the blocking postkey helper"
fi
if rg -Fq 'ieee80211_find_sta(' <<< "$fn_body"; then
	fail "stage2 calls ieee80211_find_sta() -- it must use the lifetime-safe wcid_to_sta() pattern instead"
fi

printf 'stage-2 lifetime contract passed\n'
