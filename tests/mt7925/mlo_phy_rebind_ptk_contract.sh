#!/usr/bin/env bash
# SPDX-License-Identifier: ISC

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'MLO PHY-rebind PTK contract failed: %s\n' "$1" >&2
	exit 1
}

body=$(sed -n '/^mt7927_mlo_refresh_reassigned_sta(/,/^}/p' \
	mt7925/mlo/lifecycle.c)
helper=$(sed -n '/^mt7927_mlo_recreate_authorize_peer(/,/^}/p' \
	mt7925/mlo/lifecycle.c)

[[ -n $body ]] || fail "reassigned-peer refresh function is missing"

invalidate_line=$(rg -n 'peer->wcid\.hw_key_idx = 255;' <<< "$helper" |
	head -1 | cut -d: -f1 || true)
reconcile_line=$(rg -n 'mt7925_mlo_reconcile_link\(dev, vif, sta, peer, link_id\)' \
	<<< "$helper" | head -1 | cut -d: -f1 || true)
publish_line=$(rg -n 'peer->wcid\.link_valid = true;' <<< "$helper" |
	head -1 | cut -d: -f1 || true)

[[ -n $invalidate_line ]] || fail "migrated peer PTK index is not invalidated"
[[ -n $reconcile_line ]] || fail "migrated peer PTK is not reinstalled"
[[ -n $publish_line ]] || fail "migrated peer is never republished"

(( invalidate_line < reconcile_line )) ||
	fail "PTK index must be invalidated before reconciliation"
(( reconcile_line < publish_line )) ||
	fail "peer is published before PTK reconciliation completes"

if ! rg -Uq 'mt7925_mlo_reconcile_link\(dev, vif, sta, peer, link_id\);\n[[:space:]]*if \(err\)\n[[:space:]]*return err;' \
	<<< "$body"; then
	if ! rg -Uq 'mt7925_mlo_reconcile_link\(dev, vif, sta, peer, link_id\);\n[[:space:]]*if \(READ_ONCE[^;]*;\n(?:[^\n]*\n)*?[[:space:]]*if \(err\)\n[[:space:]]*return err;' \
		<<< "$helper"; then
	fail "PTK reconciliation failure is not propagated"
	fi
fi

printf 'MLO PHY-rebind PTK contract passed\n'
