#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Required trace names for the STA-owned two-stage post-key ROC trigger,
# so a live run can be reconstructed unambiguously from dmesg.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'traces contract failed: %s\n' "$1" >&2
	exit 1
}

for trace in POSTKEY_SETKEY_ARM POSTKEY_STAGE1 POSTKEY_STAGE2 POSTKEY_KEYS_READY \
	     POSTKEY_ROC_REQ POSTKEY_ROC_RESULT POSTKEY_PREPARE POSTKEY_ACTIVATE \
	     POSTKEY_KEEP_DORMANT; do
	if ! rg -Fq "$trace" mt7925/mlo/association.c mt7925/mlo/lifecycle.c; then
		fail "required trace missing: $trace"
	fi
done

printf 'traces contract passed\n'
