#!/usr/bin/env bash
# Contract for the survivor firmware-context reprogram (mlo_survivor_rejoin
# recovery): DEV_INFO -> BSS_INFO -> BMC group keys, in normal-lifecycle
# order, before the peer stages; no DEV/BSS removal; a failed command
# fails safe without more commands; hw_key_idx invalidated before keys.
set -u
cd "$(dirname "$0")/../.." || exit 1
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
src=mt7925/mlo/lifecycle.c

f=$(sed -n '/^static int mt7927_mlo_pt_reprogram_survivor_context(/,/^}/p' $src)
[ -n "$f" ] || fail "reprogram function is missing"

d=$(grep -n 'mt76_connac_mcu_uni_add_dev' <<< "$f" | head -1 | cut -d: -f1)
b=$(grep -n 'mt7925_mcu_add_bss_info' <<< "$f" | head -1 | cut -d: -f1)
k=$(grep -n 'mt7925_mlo_group_key_rebind' <<< "$f" | head -1 | cut -d: -f1)
[ -n "$d" ] && [ -n "$b" ] && [ -n "$k" ] || fail "DEV/BSS/BMC-key commands missing"
[ "$d" -lt "$b" ] && [ "$b" -lt "$k" ] || fail "order must be DEV_INFO, BSS_INFO, BMC keys"

flat=$(tr '\n\t' '  ' <<< "$f")
grep -Eq 'mt76_connac_mcu_uni_add_dev\([^;]*, true\);' <<< "$flat" ||
	fail "DEV_INFO must be an enable (never a disable: MLD links share one OMAC)"
if grep -Eq 'uni_add_dev\([^;]*, false\)|mt792x_mac_link_bss_remove|mt7925_mcu_sta_update\(' <<< "$flat"; then
	fail "context reprogram must not remove DEV/BSS/STA"
fi
grep -q 'hw_key_idx = 255' <<< "$f" ||
	fail "stale BMC hw_key_idx must be invalidated before the key rebind"
grep -q 'mt7925_mlo_pt_ctx_next' <<< "$f" ||
	fail "step order must come from the pure ctx_next helper"
for t in MLO_SURVIVOR_CONTEXT_CREATE MLO_SURVIVOR_DEV MLO_SURVIVOR_BSS MLO_SURVIVOR_BMC MLO_SURVIVOR_GTK; do
	grep -q "$t" <<< "$f" || fail "missing trace $t"
done

h=$(sed -n '/^static int mt7927_mlo_pt_survivor_handoff(/,/^}/p' $src)
grep -q 'mt7927_mlo_pt_reprogram_survivor_context' <<< "$h" ||
	fail "handoff must call the context reprogram"
grep -q 'survivor_context_cmd_failed' <<< "$h" ||
	fail "a failed context command must fail safe"
c=$(grep -n 'mt7927_mlo_pt_reprogram_survivor_context' <<< "$h" | head -1 | cut -d: -f1)
p=$(grep -n 'mt7927_mlo_recreate_authorize_peer' <<< "$h" | head -1 | cut -d: -f1)
[ "$c" -lt "$p" ] || fail "context reprogram must precede the peer stages"
echo "survivor context reprogram contract passed"
