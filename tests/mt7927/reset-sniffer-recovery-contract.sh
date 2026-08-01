#!/bin/sh
set -eu

root=${1:?usage: reset-sniffer-recovery-contract.sh path/to/mt76}
main="$root/mt7925/main.c"
mac="$root/mt7925/mac.c"
header="$root/mt7925/mt7925.h"

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

grep -q 'struct mt7925_sniffer_iter_data' "$main" ||
	fail "missing error-carrying sniffer iterator state"
grep -q 'vif->type != NL80211_IFTYPE_MONITOR' "$main" ||
	fail "sniffer replay is not restricted to monitor vifs"
grep -q 'data->error' "$main" ||
	fail "sniffer MCU failures are not retained"
grep -q 'mt7925_sniffer_rearm' "$header" ||
	fail "missing reset-safe monitor re-arm entry point"
grep -q 'mutex_lock(&dev->mt76.mutex)' "$mac" ||
	fail "reset recovery does not serialize monitor replay"
grep -q 'monitor sniffer re-arm failed' "$mac" ||
	fail "reset recovery failure is not logged"

rearm_line=$(grep -n 'mt7925_sniffer_rearm(dev)' "$mac" | head -1 | cut -d: -f1)
wake_line=$(grep -n 'ieee80211_wake_queues(hw)' "$mac" | tail -1 | cut -d: -f1)
[ -n "$rearm_line" ] && [ -n "$wake_line" ] ||
	fail "missing reset re-arm or queue wake"
[ "$rearm_line" -lt "$wake_line" ] ||
	fail "queues wake before monitor sniffer recovery"

echo "PASS: reset sniffer recovery contract"
