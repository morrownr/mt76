#!/usr/bin/env bash
# SPDX-License-Identifier: ISC

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

body=$(sed -n '/^int mt7925_mcu_abort_mlo_roc(/,/^}/p' mt7925/mcu.c)

[[ -n $body ]] || { echo "missing mt7925_mcu_abort_mlo_roc" >&2; exit 1; }
grep -Fq '.dbdcband = 0xfe' <<<"$body" || {
	echo "MLO JOIN abort must encode BAND_ALL (0xfe)" >&2
	exit 1
}
if grep -Eq 'if .*dbdcband|mconf->mt76\.band_idx.*dbdcband|roc_mlo_links.*dbdcband' <<<"$body"; then
	echo "MLO JOIN abort selector must not vary by link or installed band" >&2
	exit 1
fi

echo "MLO JOIN abort BAND_ALL contract passed"
