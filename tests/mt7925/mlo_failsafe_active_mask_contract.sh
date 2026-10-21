#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# FAILED_SAFE active-mask contract: a replacement link that never got a
# physical grant is never reported as active. The failed grow is unwound by
# mac80211 (change_vif_links error restores the pre-grow mask); transition
# traces print the mask really in effect, the transient mac80211 value is
# labelled separately, and control-plane markers do not need the per-packet
# mlo_diag_trace.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'failsafe active-mask contract failed: %s\n' "$1" >&2
	exit 1
}

mlo=mt7925/mlo/lifecycle.c
pt=mt7925/mlo_pair_transition.h
dbg=mt7925/debugfs.c
init=mt7925/init.c

grep -q 'mt7925_mlo_pt_published_active(unsigned int vif_active' "$pt" ||
	fail "published-active helper missing"
grep -q 'vif_active & ~(added & ~granted)' "$pt" ||
	fail "ungranted added link must be masked out"

trace=$(sed -n '/^static void mt7927_mlo_pt_trace(/,/^}/p' "$mlo")
grep -q 'mt7925_mlo_pt_published_active(vif->active_links, m->pt_added' <<< "$trace" ||
	fail "transition trace must print the published mask as active_links"
grep -q 'mac80211_transient=' <<< "$trace" ||
	fail "transient mac80211 mask must be labelled separately"

# the delta acquire failure returns the error from change_vif_links so
# mac80211 restores the pre-grow active mask (no ungranted link published)
grep -q 'err = mt7927_mlo_pt_delta_acquire(dev, phy, vif, mvif,' "$mlo" ||
	fail "delta acquire call missing"
grep -A3 'err = mt7927_mlo_pt_delta_acquire(dev, phy, vif, mvif,' "$mlo" |
	grep -q 'goto err_remove_created' ||
	fail "delta acquire failure must propagate out of change_vif_links"

grep -q 'MLO_CONTINUITY_OWNER_ARMED' "$mlo" || fail "owner-armed marker missing"
grep -q 'mt7925_mlo_active_links_get' "$dbg" || fail "active-links getter missing"
grep -q '^bool mt7925_mlo_ctl_trace;' "$init" ||
	fail "mlo_ctl_trace must default off"

echo "failsafe active-mask contract passed"
