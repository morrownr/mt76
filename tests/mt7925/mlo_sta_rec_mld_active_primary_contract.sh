#!/bin/sh
# SPDX-License-Identifier: ISC
set -eu

body=$(sed -n '/^mt7925_mcu_sta_mld_tlv(/,/^}/p' mt7925/mlo/peer.c)

printf '%s' "$body" | grep -Fq \
	'!(active_links & BIT(mlink_pri->wcid.link_id))' || {
	echo "STA_REC_MLD does not replace an inactive original primary" >&2
	exit 1
}
printf '%s' "$body" | grep -Fq \
	'!mlink_pri->wcid.link_valid' || {
	echo "STA_REC_MLD can retain a dormant/unadmitted original primary" >&2
	exit 1
}
printf '%s' "$body" | grep -Fq \
	'for_each_set_bit(link_id, &active_links,' || {
	echo "STA_REC_MLD does not select its replacement from active_links" >&2
	exit 1
}
printf '%s' "$body" | grep -Fq \
	'!link_msta->wcid.link_valid' || {
	echo "STA_REC_MLD can select a non-valid replacement primary" >&2
	exit 1
}

echo "STA_REC_MLD active-primary contract passed"
