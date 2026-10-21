#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# The primary retouch sent before an incoming partner's STA_REC carries the two-link STA_REC_MLD. It must be addressed
# to a live MLD member. With a retired host deflink (0x5 -> 0x3: deflink L2 retired, survivor L0) the stock code used
# mlink->wcid.def_wcid (the retired deflink's WCID): firmware never saw an updated record for the survivor, kept it
# related to the retired WCID and left the incoming WCID unattached (UWTBL peer MLD = AP link address).

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'retouch-live-primary contract failed: %s\n' "$1" >&2
	exit 1
}

[[ -f mt7925/mlo_retouch_primary.h ]] || fail "mlo_retouch_primary.h is missing"
grep -q 'include "../mlo_retouch_primary.h"' mt7925/mlo/association.c || fail "main.c does not include the retouch resolver"

body=$(sed -n '/^static int mt7925_mac_link_sta_add(/,/^}/p' mt7925/mlo/association.c)
[[ -n $body ]] || fail "mt7925_mac_link_sta_add is missing"

grep -q 'mt7925_mlo_retouch_primary_link(' <<<"$body" ||
	fail "the primary retouch subject is not resolved to a live member"
grep -q 'mt7925_partner_retouch(dev, vif, mlink, pri_mlink' <<<"$body" ||
	fail "partner retouch call changed shape"
