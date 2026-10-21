#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mac80211's _ieee80211_set_active_links() unconditionally shrinks to
# old_active & active_links before growing to the final mask whenever the
# two masks overlap (net/mac80211/link.c:650-659, "if there's at least one
# link that stays active across the change then switch to it (to those)
# first"). For any ordinary 1-out/1-in pair swap this means the driver's
# change_vif_links callback WILL be invoked with new_links holding exactly
# one bit -- this is not a test-only shape, it is mandatory core behaviour.
#
# Proven live: old=0x6 new=0x2 (rem=0x4, add=0) reached
# mt7927_mlo_reassign_active_bands(), which hard-rejects any new_links
# whose popcount isn't 2, returning -EINVAL up through
# mt7927_mlo_change_vif_links() into drv_change_vif_links(), tripping
# WARN_ON_ONCE at net/mac80211/link.c:625.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'survivor-shrink contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
[[ -n $body ]] || fail "mt7927_mlo_change_vif_links is missing"

# 1. A dedicated branch must exist for the mandatory single-survivor shape.
if ! rg -q 'hweight16\(new_links\) == 1' <<< "$body"; then
	fail "no branch recognizes new_links with exactly one bit (the mac80211-mandated survivor phase)"
fi

survivor_body=$(awk '/^\tif \(hweight16\(new_links\) == 1/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "${body#*Phase 1: shrink}")
[[ -n $survivor_body ]] || fail "could not isolate survivor-phase branch body"

# 2. The survivor phase must not evaluate __ffs() on a mask that can be
#    empty. new_links has exactly one bit here, so
#    "new_links & ~BIT(primary)" is always 0 -- __ffs(0) is undefined.
if rg -q '__ffs\(new_links & ~BIT\(' <<< "$survivor_body"; then
	fail "survivor phase evaluates __ffs(new_links & ~BIT(primary)) on a single-bit mask -- undefined for an empty result"
fi

# 3. The survivor phase must use an explicit sentinel for "no secondary
#    link", not a computed index.
if ! rg -q 'secondary_link_id = IEEE80211_LINK_UNSPECIFIED' <<< "$survivor_body"; then
	fail "survivor phase does not set secondary_link_id to an explicit sentinel"
fi

# 4. The survivor phase must not request two-link ROC/admission -- there is
#    no substitute being admitted in this phase, only an obsolete link being
#    retired.
if rg -q 'mt7925_set_mlo_roc' <<< "$survivor_body"; then
	fail "survivor phase requests MLO ROC -- no substitute is being admitted in a shrink-only transition"
fi

# 5. The survivor phase must not touch band assignment. A lone surviving
#    link keeps whatever real RF path it already had as part of the outgoing
#    pair; mt7927_mlo_reassign_active_bands() cannot even accept a
#    single-bit mask (it requires exactly two), so it must not be called
#    from here at all.
if rg -q 'mt7927_mlo_reassign_active_bands' <<< "$survivor_body"; then
	fail "survivor phase calls mt7927_mlo_reassign_active_bands(), which hard-requires a two-link mask"
fi

# 6. The survivor phase must retire only the departing (rem) link(s) --
#    never touch the surviving link's own resident state.
if ! rg -q 'mt7927_mlo_note_bss\(mvif, mconf, mlink, false\)' <<< "$survivor_body"; then
	fail "survivor phase does not retire the obsolete link via mt7927_mlo_note_bss(..., false)"
fi

printf 'survivor-shrink contract passed\n'
