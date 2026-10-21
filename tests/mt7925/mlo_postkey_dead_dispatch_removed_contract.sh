#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mt7927_mlo_sta_postkey_dispatch() was proven unreachable: mac80211 core's
# drv_change_sta_links() discards the old==new callback one layer above any
# driver code, for any driver, before mt7925_change_sta_links() ever runs.
# The STA-owned two-stage SET_KEY trigger replaces it entirely. This
# contract pins full removal and restoration of the original behavior.
#
# It also pins removal of the OLD synchronous in-transition call to
# mt7927_mlo_postkey_attempt_locked() at the tail of
# mt7927_mlo_change_sta_links(): under the new rule that keys_seen==0 is
# TERMINAL (consumes pending, no retry), that old call site -- which fires
# before mac80211's key_switch_links() has run and therefore always saw
# keys_seen==0 -- would wrongly consume the pending record before the real
# SET_KEY-triggered stage1/stage2 path ever gets a chance to run it.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'dead dispatch removal contract failed: %s\n' "$1" >&2
	exit 1
}

# 1. No trace of the dispatch function anywhere.
if rg -q 'mt7927_mlo_sta_postkey_dispatch' mt7925/mlo/association.c mt7925/mlo/lifecycle.c mt7925/mt7925.h; then
	fail "mt7927_mlo_sta_postkey_dispatch still referenced somewhere"
fi

# 2. The STA-links old==new branch is restored to a bare return 0.
wrapper_body=$(awk '/^mt7927_mlo_assoc_change_sta_links\(/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/association.c)
[[ -n $wrapper_body ]] || fail "mt7925_change_sta_links() is missing"
oldnew_block=$(awk '/if \(old_links == new_links\)/{flag=1; print; next} flag{print; exit}' <<< "$wrapper_body")
if ! rg -Fq 'if (old_links == new_links)' <<< "$oldnew_block"; then
	fail "could not find the STA-links old==new branch"
fi
if ! rg -Fq 'return 0;' <<< "$oldnew_block"; then
	fail "STA-links old==new branch no longer returns 0 immediately"
fi

# 3. The old contract file for the removed dispatch is gone.
if [[ -f tests/mt7925/mlo_postkey_dispatch_contract.sh ]]; then
	fail "tests/mt7925/mlo_postkey_dispatch_contract.sh still exists -- should be removed with the dead code"
fi

# 4. The old synchronous in-transition attempt call is gone from
#    mt7927_mlo_change_sta_links()'s tail.
if rg -Fq 'if (!err && READ_ONCE(mt7925_test_mt7927_sublink_postkey))' mt7925/mlo/lifecycle.c; then
	fail "old synchronous in-transition postkey_attempt_locked() call site still present -- would wrongly terminal-consume pending on keys_seen==0"
fi

printf 'dead dispatch removal contract passed\n'
