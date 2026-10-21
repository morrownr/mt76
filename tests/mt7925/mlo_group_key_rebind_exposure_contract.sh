#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mt7925_mlo_group_key_rebind() was static in main.c, so mt7927_mlo.c could
# not call it directly for the pre-ROC prekey experiment without either
# duplicating its logic or exposing it narrowly. This contract requires the
# latter: a non-static declaration + header prototype, no second
# implementation, plus the new test-only flag the experimental completion
# phase gates on.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'group-key rebind exposure contract failed: %s\n' "$1" >&2
	exit 1
}

# 1. mt7925_mlo_group_key_rebind() must no longer be static in main.c.
if rg -Fq 'static int mt7925_mlo_group_key_rebind(' mt7925/mlo/association.c; then
	fail "mt7925_mlo_group_key_rebind is still static -- mt7927_mlo.c cannot call it"
fi
if ! rg -Fq 'int mt7925_mlo_group_key_rebind(' mt7925/mlo/association.c; then
	fail "mt7925_mlo_group_key_rebind definition not found in main.c"
fi

# 2. A matching prototype must exist in mlo/decls.h (the shared internal
#    header both main.c and mt7927_mlo.c already include).
if ! rg -Fq 'mt7925_mlo_group_key_rebind(' mt7925/mlo/decls.h; then
	fail "no mt7925_mlo_group_key_rebind() prototype declared in mlo/decls.h"
fi

# 3. No second implementation -- exactly one definition of the function
#    body across the tree.
def_count=$(rg -Fc 'int mt7925_mlo_group_key_rebind(struct mt792x_dev' mt7925/*.c mt7925/mlo/*.c | awk -F: '{sum+=$2} END{print sum+0}')
if (( def_count != 1 )); then
	fail "expected exactly one definition of mt7925_mlo_group_key_rebind, found $def_count"
fi

# 4. mt7927_mlo.c must actually call the shared implementation, not
#    reimplement key iteration itself.
if ! rg -Fq 'mt7925_mlo_group_key_rebind(' mt7925/mlo/lifecycle.c; then
	fail "mt7927_mlo.c never calls mt7925_mlo_group_key_rebind()"
fi
if rg -Fq 'ieee80211_iter_keys(mt76_hw(dev)' mt7925/mlo/lifecycle.c; then
	fail "mt7927_mlo.c duplicates key iteration (ieee80211_iter_keys) instead of reusing the shared helper"
fi

# 5. New test flag exists, defaults false, lives with the other MT7927 MLO
#    test flags in mt7927_mlo.c.
if ! rg -q 'static bool mt7925_test_mt7927_sublink_prekey;' mt7925/mlo/lifecycle.c; then
	fail "no mt7925_test_mt7927_sublink_prekey flag declared in mt7927_mlo.c"
fi
if ! rg -q 'module_param_named\(test_mt7927_sublink_prekey' mt7925/mlo/lifecycle.c; then
	fail "test_mt7927_sublink_prekey is not registered as a module parameter"
fi

printf 'group-key rebind exposure contract passed\n'
