#!/bin/sh

set -eu

PATH=$PATH:/usr/sbin:/sbin
export PATH

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
TOOLS=$HERE/tools
MODE=dry-run
IW_LIST=

while [ "$#" -gt 0 ]; do
	case $1 in
		--execute) MODE=execute ;;
		--iw-list) shift; [ "$#" -gt 0 ] || exit 2; IW_LIST=$1 ;;
		*) echo 'usage: run-hardware-matrix.sh [--execute] [--iw-list FILE]' >&2; exit 2 ;;
	esac
	shift
done

if [ -z "$IW_LIST" ]; then
	IW_LIST=$HERE/iw-phy-info.txt
	if [ "$MODE" = execute ]; then
		iw phy "${MONITOR_PHY:-phy0}" info >"$IW_LIST"
	elif [ ! -r "$IW_LIST" ]; then
		echo 'dry-run requires --iw-list FILE or iw-phy-info.txt' >&2
		exit 2
	fi
fi

if [ "$MODE" = execute ]; then
	MT7927_MONITOR_OWNED_LAB=yes "$TOOLS/monitor-contract.sh" \
		--all-enabled --iw-list "$IW_LIST" \
		--rounds "${MONITOR_ROUNDS:-1}" --execute
	INJECTION_OWNED_LAB=YES "$TOOLS/injection-matrix.sh" --live --iw-list "$IW_LIST"
else
	"$TOOLS/monitor-contract.sh" --all-enabled --iw-list "$IW_LIST" \
		--rounds "${MONITOR_ROUNDS:-1}" --dry-run
	"$TOOLS/injection-matrix.sh" --dry-run --iw-list "$IW_LIST"
fi
