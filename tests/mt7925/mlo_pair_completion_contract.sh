#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Rescopes the old "direct 2-out/2-in swap in one callback" make-before-break
# branch, which is unreachable through the normal steering path: mac80211's
# _ieee80211_set_active_links() only ever calls drv_change_vif_links() with
# hweight(old)==2 && hweight(new)==2 in the SAME call when old and new share
# no link at all (never the case for a 1-out/1-in pair swap, since mac80211
# always shrinks to the intersection first -- see
# mlo_survivor_shrink_contract.sh). The real second half of that swap is a
# GROWTH-ONLY callback: old_links has exactly one bit (the survivor from the
# shrink phase), new_links has two, rem is empty. This is the shape that
# actually reaches the driver, proven live (old=0x2 new=0x3 add=0x1 rem=0x0
# immediately following the survivor-phase call).

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'pair-completion contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
[[ -n $body ]] || fail "mt7927_mlo_change_vif_links is missing"

# 1. The unreachable direct 2-out/2-in single-call shape must be gone --
#    do not leave dead behaviour presented as active protection.
if rg -q 'hweight16\(old_links\) == 2 && hweight16\(new_links\) == 2' <<< "$body"; then
	fail "the unreachable direct 2-in/2-out swap branch (hweight(old)==2 && hweight(new)==2) is still present"
fi

# 2. A dedicated branch must exist for the real completion shape: growing
#    from the lone survivor to the final two-link pair.
if ! rg -q 'hweight16\(old_links\) == 1 && hweight16\(new_links\) == 2' <<< "$body"; then
	fail "no branch recognizes the old==1/new==2 completion phase"
fi

completion_body=$(awk '/^\tif \(hweight16\(old_links\) == 1 && hweight16\(new_links\) == 2/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "$body")
[[ -n $completion_body ]] || fail "could not isolate completion-phase branch body"

# 3. Must reuse/admit the substitute link.
if ! rg -q 'mt7927_mlo_add_bss_link' <<< "$completion_body"; then
	fail "completion phase does not admit/reuse the substitute link via mt7927_mlo_add_bss_link()"
fi

# 4. Must assign the two physical band slots for the final pair.
if ! rg -q 'mt7927_mlo_reassign_active_bands' <<< "$completion_body"; then
	fail "completion phase does not assign final two-link band contexts"
fi

# 5. Must run pair ROC/admission for the final mask, anchored on the
#    resident survivor -- never on &mvif->bss_conf directly (that object is
#    the vif's ORIGINAL deflink from true initial association, stale once
#    that link has been retired; see mlo_roc_survivor_anchor_contract.sh).
if ! rg -q 'mt7925_set_mlo_roc\(phy, anchor_mconf, new_links\)' <<< "$completion_body"; then
	fail "completion phase does not request MLO ROC anchored on the resident survivor (anchor_mconf)"
fi
if rg -q 'mt7925_set_mlo_roc\(phy, &mvif->bss_conf, new_links\)' <<< "$completion_body"; then
	fail "completion phase still anchors ROC on &mvif->bss_conf directly -- stale-deflink bug is back"
fi

# 6. Must not commit externally-visible pair bookkeeping
#    (traffic_active_links/primary_link_id/secondary_link_id) before the
#    band reassignment and ROC have both been attempted -- a failure must
#    never be able to leave mac80211 and the driver disagreeing about which
#    pair is active.
band_line=$(grep -n 'mt7927_mlo_reassign_active_bands' <<< "$completion_body" | head -1 | cut -d: -f1)
roc_line=$(grep -n 'mt7925_set_mlo_roc(phy, anchor_mconf, new_links)' <<< "$completion_body" | head -1 | cut -d: -f1)
commit_line=$(grep -n 'traffic_active_links = new_links' <<< "$completion_body" | head -1 | cut -d: -f1)

[[ -n $band_line && -n $roc_line && -n $commit_line ]] || fail "could not locate band/ROC/commit lines to order-check"

if (( commit_line < band_line )); then
	fail "traffic_active_links is committed (line $commit_line) before band reassignment (line $band_line)"
fi
if (( commit_line < roc_line )); then
	fail "traffic_active_links is committed (line $commit_line) before ROC admission (line $roc_line)"
fi

printf 'pair-completion contract passed\n'
