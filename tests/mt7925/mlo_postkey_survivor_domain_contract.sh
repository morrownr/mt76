#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mt7927_mlo_postkey_attempt_locked() previously resolved the survivor via
# mt7927_mlo_resident_sta(dev, &mvif->sta, pend_surv) -- the VIF/BMC
# pseudo-STA's own per-link mlink, not the peer's. Gating survivor validity
# on that object's wcid.link_valid checks the wrong domain: a BMC WCID
# cannot stand in for the peer's own per-link admission state. This
# contract pins the fix: the survivor lookup must use the peer msta, the
# same object the substitute lookup already (correctly) uses.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'survivor domain contract failed: %s\n' "$1" >&2
	exit 1
}

fn_body=$(awk '/mt7927_mlo_postkey_attempt_locked\(struct mt792x_dev/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $fn_body ]] || fail "mt7927_mlo_postkey_attempt_locked() is missing"

# 1. Survivor resolution must use msta, never &mvif->sta.
if rg -Fq 'mt7927_mlo_resident_sta(dev, &mvif->sta,' <<< "$fn_body"; then
	fail "survivor lookup still uses the BMC pseudo-STA (&mvif->sta) -- must use the peer msta"
fi
if ! rg -Fq 'surv_mlink = mt7927_mlo_resident_sta(dev, msta,' <<< "$fn_body"; then
	fail "survivor lookup does not resolve via the peer msta"
fi

# 2. Substitute lookup (prepare path) must also use msta, not &mvif->sta --
#    consistency check that the fix didn't invert the bug.
if rg -q '(sub_mlink|mlink) = mt7927_mlo_resident_sta\(dev, &mvif->sta,' <<< "$fn_body"; then
	fail "substitute-side lookup regressed to using the BMC pseudo-STA"
fi

printf 'survivor domain contract passed\n'
