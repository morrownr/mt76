#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'MLO peer authorization lifecycle contract failed: %s\n' "$1" >&2
	exit 1
}

mcu=$(sed -n '/^int mt7925_mcu_sta_rebind_phy(/,/^}/p' mt7925/mcu.c)
helper=$(sed -n '/^mt7927_mlo_recreate_authorize_peer(/,/^}/p' mt7925/mlo/lifecycle.c)
refresh=$(sed -n '/^mt7927_mlo_refresh_reassigned_sta(/,/^}/p' mt7925/mlo/lifecycle.c)

rg -q 'MT76_STA_INFO_STATE_NONE, true' <<< "$mcu" ||
	fail 'destructive rebind is not STATE_NONE,newly=1'
rg -q 'mt7925_mcu_sta_authorize' <<< "$helper" ||
	fail 'destructive migration has no terminal ordinary authorization'
if rg -q 'peer->wcid\.link_valid = false' <<< "$helper"; then
	fail 'link_valid is incorrectly used as the migration TX gate and suppresses STA_REC_MLD'
fi
rg -q 'msta->mlo_link_ready_jiffies\[link_id\] = 0' <<< "$refresh" ||
	fail 'migration does not withhold TX through the readiness gate'

create_line=$(rg -n 'mt7925_mcu_sta_rebind_phy' <<< "$helper" | head -1 | cut -d: -f1)
restore_line=$(rg -n 'mt7925_mlo_reconcile_link' <<< "$helper" | head -1 | cut -d: -f1)
authorize_line=$(rg -n 'mt7925_mcu_sta_authorize' <<< "$helper" | head -1 | cut -d: -f1)
publish_line=$(rg -n 'peer->wcid\.link_valid = true' <<< "$helper" | head -1 | cut -d: -f1)
ready_line=$(rg -n 'mlo_link_ready_jiffies\[link_id\] = jiffies' <<< "$refresh" | head -1 | cut -d: -f1)

(( create_line < restore_line && restore_line < authorize_line &&
   authorize_line < publish_line && publish_line < ready_line )) ||
	fail 'required create -> restore -> authorize -> publish -> ready order is absent'

rg -Uq 'mt7925_mcu_sta_authorize[^;]*;\n[[:space:]]*if \(READ_ONCE' <<< "$helper" ||
	fail 'authorization result is not captured before publication'
rg -Uq 'stage=%s[^;]*;\n[[:space:]]*if \(err\)\n[[:space:]]*return err;' <<< "$helper" ||
	fail 'authorization failure is not propagated'

retry=$(sed -n '/for (attempt = 0;/,/^[[:space:]]*}/p' <<< "$refresh")
rg -q 'mt7927_mlo_recreate_authorize_peer' <<< "$retry" ||
	fail 'convergence retry does not run the complete recreate/authorize lifecycle'

printf 'MLO peer authorization lifecycle contract passed\n'
