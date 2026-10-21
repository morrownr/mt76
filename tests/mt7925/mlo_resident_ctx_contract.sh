#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# A link whose structures stay resident can have lost its firmware BSS (an earlier
# shrink removed it and cleared its vif-mask bit). Hardware (Oct 19): the JOIN built
# for such a link was refused by the ROC request builder (-ENXIO, bss_idx not in the
# vif mask) and the reassociation ended on a single link. Re-entering a resident link
# must restore the firmware context on the same structures, and must not record the
# link as newly created (the error unwind frees created links).

set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
fail() { printf 'mlo-resident-ctx contract failed: %s\n' "$1" >&2; exit 1; }

f=$(sed -n '/^mt7927_mlo_add_bss_link(/,/^}/p' mt7925/mlo/lifecycle.c)
[[ -n $f ]] || fail "mt7927_mlo_add_bss_link not found"
r=$(sed -n '/if (mconf && mlink) {/,/^	}/p' <<< "$f")
[[ -n $r ]] || fail "no resident branch"
grep -q 'mt7927_mlo_link_ctx_installed(dev, mvif, link_id)' <<< "$r" || fail "resident re-entry does not check the firmware context"
grep -q 'mt7925_mac_link_bss_add(dev, link_conf, mlink)' <<< "$r" || fail "resident re-entry does not restore the firmware BSS"
grep -q '\*created' <<< "$r" && fail "a restored resident link must not be recorded as newly created"
# the early return for an installed context is kept
grep -q 'mt7927_mlo_note_bss(mvif, mconf, mlink, true)' <<< "$r" || fail "installed resident link lost its bookkeeping"
printf 'mlo-resident-ctx contract: PASS\n'
