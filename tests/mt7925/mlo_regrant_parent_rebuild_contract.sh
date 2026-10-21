#!/usr/bin/env bash
# Contract for the post-regrant parent rebuild (mlo_regrant_parent_rebuild):
# off by default, runs inside the security rebind BEFORE any child security
# phase, rebuilds DEV_INFO then BSS_INFO for BOTH active links with the normal
# enable re-send (no DEV disable: shared OMAC), traces every command, blocks
# the child phases if the parent pass fails, and adds no new firmware ABI,
# JOIN shape or teardown.
set -u
cd "$(dirname "$0")/../.." || exit 1
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

src=mt7925/mlo/lifecycle.c

grep -Eq '^static bool mt7925_mlo_regrant_parent_rebuild;' $src ||
	fail "mlo_regrant_parent_rebuild must default to N"
grep -q 'module_param_named(mlo_regrant_parent_rebuild' $src ||
	fail "mlo_regrant_parent_rebuild module parameter is missing"

f=$(sed -n '/^static int mt7927_mlo_regrant_parent_rebuild(/,/^}/p' $src)
[ -n "$f" ] || fail "parent rebuild function is missing"
grep -q 'mt7925_mlo_pt_parent_next' <<< "$f" || fail "must use pure helper mt7925_mlo_pt_parent_next"
grep -q 'mt76_connac_mcu_uni_add_dev(.*' <<< "$f" || fail "must re-send DEV_INFO"
grep -q 'mt7925_mcu_add_bss_info(' <<< "$f" || fail "must re-send BSS_INFO"
grep -q 'MLO_REGRANT_DEV_REBUILD' <<< "$f" && grep -q 'MLO_REGRANT_BSS_REBUILD' <<< "$f" ||
	fail "DEV/BSS rebuild must be traced"
grep -q ', false)' <<< "$f" && fail "parent rebuild must not disable DEV/BSS"
if grep -Eq 'mt7925_set_mlo_roc|mt7925_mcu_set_roc|ieee80211_disconnect|ieee80211_connection_loss|cfg80211_disconnected|mt7925_mcu_sta_update|ptk_rebind|group_key_rebind' <<< "$f"; then
	fail "parent pass must not touch ROC, teardown or child security objects"
fi

r=$(sed -n '/^static int mt7927_mlo_security_rebind_pair(/,/^}/p' $src)
p=$(grep -n 'mt7927_mlo_regrant_parent_rebuild(dev' <<< "$r" | head -1 | cut -d: -f1)
c=$(grep -n 'mt7925_mlo_pt_rebind_next(ph, 0)' <<< "$r" | head -1 | cut -d: -f1)
[ -n "$p" ] && [ -n "$c" ] && [ "$p" -lt "$c" ] ||
	fail "parent rebuild must run before the child security phases"
grep -q 'mt7925_mlo_pt_child_allowed' <<< "$r" || fail "child phases must be gated on parent DONE"
grep -q 'mt7925_mlo_pt_parent_links' <<< "$r" || fail "parent links must come from the pure helper"
echo "PASS mlo_regrant_parent_rebuild_contract"
