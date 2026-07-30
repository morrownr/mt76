#!/bin/sh
# SPDX-License-Identifier: ISC
set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
main="$root/mt7925/main.c"
mac="$root/mt7925/mac.c"

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

iterator=$(
	perl -0ne '
		if (/mt7925_sniffer_interface_iter\s*\([^)]*\)\s*\{(.*?)\n\}/s) {
			print $1;
		}
	' "$main"
)

[ -n "$iterator" ] || fail "cannot locate mt7925_sniffer_interface_iter"

printf '%s\n' "$iterator" |
	grep -q 'vif->type == NL80211_IFTYPE_MONITOR' ||
	fail "reset replay does not derive monitor state from the monitor vif"

printf '%s\n' "$iterator" |
	grep -q 'mt7925_mcu_config_sniffer(mvif, ctx)' ||
	fail "reset replay does not configure the sniffer channel"

if printf '%s\n' "$iterator" |
	grep -B2 -A2 'mt7925_mcu_config_sniffer(mvif, ctx)' |
	grep -q 'is_mt7927'; then
	fail "sniffer channel configuration is incorrectly limited to MT7927"
fi

grep -q 'mt7925_sniffer_rearm(dev)' "$mac" ||
	fail "chip-reset recovery does not invoke monitor sniffer replay"

echo "PASS: PR #69 reset replay is vif-driven and configures both chip families"
