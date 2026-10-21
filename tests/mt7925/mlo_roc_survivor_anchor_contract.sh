#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mt7925_mcu_set_mlo_roc()'s ACQUIRE (anchor) slot must be a link firmware
# already has a live JOIN history for. mvif->bss_conf.link_id is the vif's
# ORIGINAL deflink from true initial association and is never reassigned by
# this driver's MT7927 pair-transition path -- once the survivor-shrink
# phase retires that original deflink, passing &mvif->bss_conf straight
# into mt7925_set_mlo_roc() feeds mt7925_mcu_set_mlo_roc()'s own
# stale-deflink fallback (__ffs(sel_links)), which silently anchors on
# the brand-new, RF-unproven substitute instead of the real resident
# survivor. Proven live: token=4 anchored on the just-created link0
# received zero ROCEV of any kind across the full 5s wait -- total
# firmware silence, not a rejection.
#
# The fix resolves the anchor explicitly from the survivor
# (old_links & new_links, guaranteed exactly one bit by this branch's own
# shape guard) via mt7927_mlo_resident_bss()/_sta(), and never mutates
# mvif->bss_conf.link_id itself -- that field's identity (BSS index, OMAC,
# key state, RSSI) is used elsewhere and is not a knob for ROC anchor
# selection.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'ROC survivor-anchor contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^}/p' mt7925/mlo/lifecycle.c)
[[ -n $body ]] || fail "mt7927_mlo_change_vif_links is missing"

completion_body=$(awk '/^\tif \(hweight16\(old_links\) == 1 && hweight16\(new_links\) == 2/{flag=1} flag{print} flag && /^\t\}$/{exit}' <<< "$body")
[[ -n $completion_body ]] || fail "could not isolate completion-phase branch body"

# 1. The anchor must be resolved from the survivor mask (old_links &
#    new_links -- which this branch's own guard collapses to old_links,
#    since rem==0 here means old_links is entirely retained in new_links),
#    not from __ffs(new_links) or mvif->bss_conf.link_id.
if ! rg -q 'anchor_id = __ffs\(old_links\)' <<< "$completion_body"; then
	fail "anchor link is not resolved from the survivor (old_links), which this branch's shape guarantees is the sole survivor bit"
fi

# 2. The anchor's resident BSS/STA objects must be looked up explicitly --
#    never assumed to be &mvif->bss_conf.
if ! rg -q 'anchor_mconf = mt7927_mlo_resident_bss\(dev, mvif, anchor_id\)' <<< "$completion_body"; then
	fail "anchor's resident BSS object is not resolved via mt7927_mlo_resident_bss()"
fi
if ! rg -q 'anchor_mlink = mt7927_mlo_resident_sta\(dev, &mvif->sta, anchor_id\)' <<< "$completion_body"; then
	fail "anchor's resident STA object is not resolved via mt7927_mlo_resident_sta()"
fi

# 3. The anchor must be validated (exists, resident, still active/valid)
#    BEFORE any traffic-eligibility-affecting call -- i.e. before
#    mt7927_mlo_add_bss_link() creates the substitute. No valid anchor must
#    mean an error return, not a silent __ffs(new_links) fallback.
validate_line=$(grep -Fn '!anchor_mconf' <<< "$completion_body" | head -1 | cut -d: -f1)
add_link_line=$(grep -n 'mt7927_mlo_add_bss_link' <<< "$completion_body" | head -1 | cut -d: -f1)
[[ -n $validate_line ]] || fail "no validation of anchor_mconf/anchor_mlink before use"
[[ -n $add_link_line ]] || fail "completion phase does not call mt7927_mlo_add_bss_link()"
if (( validate_line > add_link_line )); then
	fail "anchor validation (line $validate_line) happens after the substitute is already admitted (line $add_link_line) -- must fail fast before creating the substitute"
fi

# 4. Invalid anchor must produce an error, not proceed.
validate_block=$(awk -v n="$validate_line" 'NR>=n && NR<=n+3' <<< "$completion_body")
if ! rg -q 'err = -EINVAL' <<< "$validate_block"; then
	fail "anchor validation does not set an error result on failure"
fi
if ! rg -q 'goto err_remove_created' <<< "$validate_block"; then
	fail "anchor validation does not bail out (goto err_remove_created) on failure"
fi

# 5. mvif->bss_conf.link_id must never be assigned anywhere in this file --
#    it is shared object identity, not a variable to relabel for ROC
#    anchor selection.
if rg -q 'bss_conf\.link_id\s*=' mt7925/mlo/lifecycle.c; then
	fail "mt7925/mlo/lifecycle.c assigns bss_conf.link_id -- must never relabel the embedded deflink object"
fi

printf 'ROC survivor-anchor contract passed\n'
