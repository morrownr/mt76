#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# .set_key()'s own successful group-key install is the SET_KEY trigger:
# after mt7925_set_link_key() succeeds for a pending substitute's group
# key, arm stage 1 using a lifetime-safe RCU-scoped peer lookup. RCU must
# be held only long enough to queue the work item -- no ROC, wait, or
# mutex recursion may happen inside .set_key().

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'SET_KEY trigger contract failed: %s\n' "$1" >&2
	exit 1
}

fn_body=$(awk '/^int mt7927_mlo_assoc_set_key\(/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/association.c)
[[ -n $fn_body ]] || fail "mt7925_set_key() is missing"

for cond in 'IEEE80211_KEY_FLAG_PAIRWISE' 'READ_ONCE(mt7925_test_mt7927_sublink_postkey)' 'READ_ONCE(mt7925_test_mt7927_sublink_roc)' 'postkey_pending' 'postkey_roc_attempted' 'postkey_generation' 'postkey_sublink_id'; do
	if ! rg -Fq "$cond" <<< "$fn_body"; then
		fail "mt7925_set_key() SET_KEY trigger does not check: $cond"
	fi
done

# RCU-scoped peer lookup: ieee80211_find_sta(), not the function's own
# msta/mlink (which is the BMC pseudo-STA for the sta==NULL group-key
# case).
if ! rg -Fq 'rcu_read_lock();' <<< "$fn_body"; then
	fail "mt7925_set_key() does not take rcu_read_lock() for the peer lookup"
fi
if ! rg -Fq 'ieee80211_find_sta(vif, vif->cfg.ap_addr)' <<< "$fn_body"; then
	fail "mt7925_set_key() does not use ieee80211_find_sta(vif, vif->cfg.ap_addr) for the peer lookup"
fi
if ! rg -Fq 'rcu_read_unlock();' <<< "$fn_body"; then
	fail "mt7925_set_key() does not release RCU after the peer lookup"
fi

# Stage 1 must be queued while still inside the RCU section (between the
# find_sta call and rcu_read_unlock), not after.
find_line=$(grep -Fn 'ieee80211_find_sta(vif, vif->cfg.ap_addr)' <<< "$fn_body" | head -1 | cut -d: -f1)
if ! rg -Fq '&pmsta->postkey_stage1);' <<< "$fn_body"; then
	fail "mt7925_set_key() does not queue &pmsta->postkey_stage1"
fi
queue_line=$(grep -Fn 'wiphy_work_queue(hw->wiphy,' <<< "$fn_body" | head -1 | cut -d: -f1 || true)
unlock_line=$(grep -Fn 'rcu_read_unlock();' <<< "$fn_body" | tail -1 | cut -d: -f1)
[[ -n $queue_line ]] || fail "mt7925_set_key() does not queue postkey_stage1 via wiphy_work_queue()"
if (( queue_line < find_line || queue_line > unlock_line )); then
	fail "postkey_stage1 is queued (line $queue_line) outside the RCU section (find=$find_line, unlock=$unlock_line)"
fi

# No blocking/MCU/ROC/mutex-recursion operation anywhere in this function.
for forbidden in 'mt7925_set_sublink_roc(' 'wait_event' 'mt792x_mutex_acquire(dev); mt792x_mutex_acquire' 'mt7927_mlo_postkey_attempt_locked('; do
	if rg -Fq "$forbidden" <<< "$fn_body"; then
		fail "mt7925_set_key() contains a forbidden blocking/recursive operation: $forbidden"
	fi
done

printf 'SET_KEY trigger contract passed\n'
