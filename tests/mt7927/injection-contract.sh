#!/bin/sh
set -eu

TEST_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=tests/mt7927/lib-injection-contract.sh
. "$TEST_DIR/lib-injection-contract.sh"

[ "$#" -eq 7 ] || {
	printf 'usage: injection-contract.sh PROBE.tsv SEND.tsv RECEIVER.tsv REFLECTION.tsv WINDOW.tsv METADATA.tsv RUN_ID\n' >&2
	exit 2
}

probe=$1
send=$2
receiver=$3
reflection=$4
window=$5
metadata=$6
run_id=$7
[ -r "$probe" ] || injection_die "probe manifest is unreadable: $probe"
[ -r "$send" ] || injection_die "send results are unreadable: $send"
[ -r "$receiver" ] || injection_die "receiver evidence is unreadable: $receiver"
[ -r "$reflection" ] || injection_die "local reflection is unreadable: $reflection"
[ -r "$window" ] || injection_die "window metadata is unreadable: $window"
[ -r "$metadata" ] || injection_die "run metadata is unreadable: $metadata"

injection_validate_evidence "$probe" "$send" "$receiver" "$reflection" "$window" "$metadata" "$run_id"
