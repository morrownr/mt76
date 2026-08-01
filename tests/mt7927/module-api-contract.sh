#!/bin/sh
set -eu

root=${1:?usage: module-api-contract.sh path/to/mt76}
core=$root/tx.c
consumer=$root/mt7925/main.c

symbol=mt76_txq_schedule_pending

if ! grep -Eq "[[:space:]]$symbol\(" "$consumer"; then
	printf 'PASS: MT7927 module API contract (helper unused)\n'
	exit 0
fi

if ! grep -Eq "EXPORT_SYMBOL_GPL\($symbol\);" "$core"; then
	printf 'FAIL: %s is called by mt7925-common.ko but is not exported by mt76.ko\n' \
		"$symbol" >&2
	exit 1
fi

printf 'PASS: MT7927 module API contract\n'
