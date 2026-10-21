#!/bin/sh
# SPDX-License-Identifier: ISC
set -eu

body=$(sed -n '/if (target == -1) {/,/return &forced->wcid;/p' mt7925/mlo/tx.c)

printf '%s' "$body" | grep -Fq 'next = (next + 1) % IEEE80211_MLD_MAX_NUM_LINKS' || {
	echo "default TID scheduler does not rotate across active links" >&2
	exit 1
}
if printf '%s' "$body" | grep -Eq 'best_rssi|rssi > best_rssi'; then
	echo "default TID scheduler collapses all TIDs onto the strongest link" >&2
	exit 1
fi
printf '%s' "$body" | grep -Fq 'mlo_tid_link_valid[tid] = true' || {
	echo "default scheduler does not preserve per-TID pinning" >&2
	exit 1
}

echo "default per-TID balance contract passed"
