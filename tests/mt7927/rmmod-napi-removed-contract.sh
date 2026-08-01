#!/bin/sh
# SPDX-License-Identifier: ISC
set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
dma="$root/mt792x_dma.c"

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

extract_func()
{
	perl -0ne '
		my $name = $ENV{"FUNC"};
		my $src = $_;
		if ($src =~ /(int\s+\Q$name\E\s*\([^)]*\)\s*\{.*?\n\})\nEXPORT_SYMBOL_GPL\(\Q$name\E\);/s) {
			print $1;
		}
	' < "$dma"
}

tx=$(FUNC=mt792x_poll_tx extract_func)
rx=$(FUNC=mt792x_poll_rx extract_func)

[ -n "$tx" ] || fail "cannot locate mt792x_poll_tx"
[ -n "$rx" ] || fail "cannot locate mt792x_poll_rx"

printf '%s\n' "$tx" |
	grep -q 'test_bit(MT76_REMOVED, &dev->mphy.state)' ||
	fail "TX NAPI poll does not bail out after device removal"

printf '%s\n' "$rx" |
	grep -q 'test_bit(MT76_REMOVED, &dev->mphy.state)' ||
	fail "RX NAPI poll does not bail out after device removal"

printf '%s\n' "$rx" |
	awk '
		/test_bit\(MT76_REMOVED/ { seen_removed = NR }
		/mt76_dma_rx_poll/ { seen_dma = NR }
		END { exit !(seen_removed && seen_dma && seen_removed < seen_dma) }
	' ||
	fail "RX removal check must run before mt76_dma_rx_poll()"

echo "PASS: mt792x NAPI poll exits cleanly after device removal"
