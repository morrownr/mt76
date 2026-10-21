#!/bin/sh
set -eu

src=${1:-mt7925/mlo/lifecycle.c}
body=$(sed -n '/^mt7927_mlo_deactivate_wcid(/,/^}/p' "$src")

printf '%s\n' "$body" | grep -q 'mt76_connac_free_pending_tx_skbs'
printf '%s\n' "$body" | grep -q 'wcid->link_valid = false'

if printf '%s\n' "$body" | grep -Eq 'mt76_wcid_cleanup|mt76_wcid_init|hw_key_idx[[:space:]]*='; then
	echo "resident WCID deactivation destroys key/WCID state" >&2
	exit 1
fi
