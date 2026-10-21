#!/usr/bin/env bash
# SPDX-License-Identifier: ISC

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'STA_REC_MLD active-link contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^mt7925_mcu_sta_mld_tlv(/,/^}/p' mt7925/mlo/peer.c)

[[ -n $body ]] || fail "MLD TLV builder is missing"

if ! rg -q 'for_each_set_bit\(link_id, &active_links' <<< "$body"; then
	fail "MLD TLV does not enumerate the complete active link set"
fi

if ! rg -q 'active_links = vif->active_links' <<< "$body"; then
	fail "MLD TLV is not built from the pair published by mac80211"
fi

if rg -q 'mlink && mlink != &msta->deflink' <<< "$body"; then
	fail "MLD TLV still depends on which single link triggered the update"
fi

printf 'STA_REC_MLD active-link contract passed\n'
