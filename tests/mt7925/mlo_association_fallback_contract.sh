#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# The second caller's pair (severity decision) at association time must never make
# an association fail that the original choice would have completed: when its pair does
# not come up the driver falls back, once, to the original (automatic) pair.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail() { printf 'mlo-association-fallback contract failed: %s\n' "$1" >&2; exit 1; }

a=$(sed -n '/^mt7925_mac_set_links(/,/^}/p' mt7925/mlo/association.c)
[[ -n $a ]] || fail "association pair selection not found"
grep -q 'goto retry' <<< "$a" || fail "no fallback retry"
grep -q 'auto_links' <<< "$a" || fail "original pair is not kept"
[[ $(grep -c 'goto fail' <<< "$a") -ge 4 ]] || fail "not every failing step reaches the fallback"
grep -q 'override_taken = false' <<< "$a" || fail "fallback can loop"
grep -q 'explicit_pair' <<< "$a" || fail "explicit diagnostic pair handling is gone"

# the fallback lives in the driver only: no generic dependency is touched for it
grep -rq 'link_addr_keep' mt7925 && fail "driver tree references the local-only mac80211 patch"

# The association-time JOIN deferral and the ROC request builder use ONE readiness
# predicate for the partner link, so a JOIN is never built for a link the builder
# will refuse (hardware: a stale partner config after a shrink, JOIN rejected -ENXIO).
b=$(sed -n '/^int mt7927_mlo_radio_plan(/,/^}/p' mt7925/mlo/radio.c)
grep -q 'mt7927_mlo_link_ctx_installed(' <<< "$b" || fail "request builder does not use the shared readiness predicate"
a=$(sed -n '/^mt7925_mac_set_links(/,/^}/p' mt7925/mlo/association.c)
grep -q 'mt7927_mlo_link_ctx_installed(dev, mvif, secondary_link_id)' <<< "$a" || fail "JOIN deferral does not use the shared readiness predicate"

printf 'mlo-association-fallback contract: PASS\n'
