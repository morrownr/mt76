#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Stage 1 runs as a wiphy_work item, guaranteed by cfg80211's single
# per-wiphy work pump to execute only after the currently-running wiphy
# work (ieee80211_activate_links_work(), the entire key-switch loop) has
# fully returned. Stage 1 must do nothing but queue stage 2 and return --
# no MCU command, ROC, wait, or mt76 mutex acquisition, so the wiphy mutex
# it runs under is never held for anything blocking.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'stage-1 barrier contract failed: %s\n' "$1" >&2
	exit 1
}

fn_body=$(awk '/^void mt7927_mlo_postkey_stage1_work\(/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $fn_body ]] || fail "mt7927_mlo_postkey_stage1_work() is missing"

if ! rg -Fq 'container_of(work, struct mt792x_sta,' <<< "$fn_body"; then
	fail "stage1 does not derive msta via container_of() from the wiphy_work"
fi
if ! rg -Fq 'postkey_stage1);' <<< "$fn_body"; then
	fail "stage1's container_of() does not name the postkey_stage1 member"
fi
if ! rg -Fq 'ieee80211_queue_work(' <<< "$fn_body"; then
	fail "stage1 does not queue stage2 via ieee80211_queue_work()"
fi
if ! rg -Fq '&msta->postkey_stage2' <<< "$fn_body"; then
	fail "stage1 does not queue msta->postkey_stage2"
fi

for forbidden in 'mt7925_set_sublink_roc(' 'mt7925_mlo_group_key_rebind(' 'wait_event' 'mt792x_mutex_acquire(' 'mt7925_mcu_' 'mt7927_mlo_postkey_attempt_locked('; do
	if rg -Fq "$forbidden" <<< "$fn_body"; then
		fail "stage1 performs a forbidden operation: $forbidden -- stage1 must only queue stage2 and return"
	fi
done

printf 'stage-1 barrier contract passed\n'
