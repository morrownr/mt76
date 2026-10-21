#!/usr/bin/env bash
# SPDX-License-Identifier: ISC

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

body=$(awk '/^mt7927_mlo_reassign_band\(/{found=1} found{print} found && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $body ]] || { echo "missing mt7927_mlo_reassign_band" >&2; exit 1; }

if rg -q 'mt76_connac_mcu_uni_add_dev\([^;]*false\)' <<<"$(tr '\n' ' ' <<<"$body")"; then
	echo "band reassignment destructively disables the shared MLD device" >&2
	exit 1
fi

count=$(rg -c 'mt76_connac_mcu_uni_add_dev\(' <<<"$body")
[[ $count -eq 1 ]] || {
	echo "band reassignment must issue exactly one in-place DEV/BSS update, found $count" >&2
	exit 1
}

rg -q 'mconf->mt76.band_idx = old_band' <<<"$body" || {
	echo "band reassignment lacks host-state rollback" >&2
	exit 1
}

echo "in-place band reassignment contract passed"
