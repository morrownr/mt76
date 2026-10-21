#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# During change_sta_links(), vif->active_links still contains the already
# published survivor set while the newly added link's STA_REC is built.  The
# STA_REC_MLD topology must therefore include the link currently being updated;
# otherwise firmware receives link_num=1 and the new WCID can complete TXFREE
# without ever gaining RF airtime.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'in-flight STA_REC_MLD link contract failed: %s\n' "$*" >&2
	exit 1
}

body=$(sed -n '/^mt7925_mcu_sta_mld_tlv(/,/^}/p' mt7925/mlo/peer.c)
[[ -n $body ]] || fail 'mt7925_mcu_sta_mld_tlv() is missing'

published=$(grep -nF 'unsigned long active_links = vif->active_links;' <<< "$body" |
	head -1 | cut -d: -f1 || true)
inflight=$(grep -nF 'active_links |= BIT(mlink->wcid.link_id);' <<< "$body" |
	head -1 | cut -d: -f1 || true)
consume=$(grep -nF 'active_links &= ~BIT(mlink_pri->wcid.link_id);' <<< "$body" |
	head -1 | cut -d: -f1 || true)

[[ -n $published ]] || fail 'published active-link seed is missing'
[[ -n $inflight ]] || fail 'the currently updated link is not added to STA_REC_MLD topology'
[[ -n $consume ]] || fail 'primary removal from the secondary enumeration is missing'

if (( inflight <= published || inflight >= consume )); then
	fail 'the in-flight link must be included after seeding and before topology enumeration'
fi

printf 'in-flight STA_REC_MLD link contract passed\n'
