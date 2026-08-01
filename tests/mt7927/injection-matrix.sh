#!/bin/sh
set -eu

TEST_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=tests/mt7927/lib-injection-contract.sh
. "$TEST_DIR/lib-injection-contract.sh"

usage()
{
	cat <<'EOF'
usage: injection-matrix.sh [--dry-run|--live] [--iw-list FILE]

Required configuration:
  INJECTION_IFACE          monitor-mode transmitter interface
  INJECTION_RECEIVER       owned receiver identifier (usually MAC address)
  INJECTION_CHANNELS       space-separated frequencies, or auto
  INJECTION_INJECT_CMD     command accepting: iface receiver tag class count
  INJECTION_CAPTURE_CMD    command accepting: frequency output-tsv
  INJECTION_STATUS_CMD     command accepting: output-tsv

Live mode additionally requires INJECTION_OWNED_LAB=YES. External commands
must append receiver and TX-status evidence using the TSV schemas printed by
--dry-run. The harness never selects disabled channels and never transmits in
dry-run mode.
EOF
}

mode=dry-run
iw_list=
while [ "$#" -gt 0 ]; do
	case $1 in
		--dry-run) mode=dry-run ;;
		--live) mode=live ;;
		--iw-list) shift; [ "$#" -gt 0 ] || { usage >&2; exit 2; }; iw_list=$1 ;;
		--help) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
	esac
	shift
done

: "${INJECTION_IFACE:=mon0}"
: "${INJECTION_RECEIVER:=00:00:00:00:00:00}"
: "${INJECTION_CHANNELS:=auto}"
: "${INJECTION_UNICAST_COUNT:=100}"
: "${INJECTION_BROADCAST_COUNT:=100}"
: "${INJECTION_ROUNDS:=50}"
: "${INJECTION_MIN_DELIVERY:=95}"
: "${INJECTION_IW:=iw}"
: "${INJECTION_INJECT_CMD:=injection-send-frame}"
: "${INJECTION_CAPTURE_CMD:=injection-receiver-capture}"
: "${INJECTION_STATUS_CMD:=injection-tx-status}"
: "${INJECTION_WORKTREE:=/home/pi/wireless-next-mt7927/.worktrees/mt7927-monitor-rearm-v3-transaction}"

[ -e "$INJECTION_WORKTREE/.git" ] || injection_die "target worktree not found: $INJECTION_WORKTREE"
target_commit=$(git -C "$INJECTION_WORKTREE" rev-parse HEAD)

if [ -n "$iw_list" ]; then
	legal=$(injection_legal_frequencies "$iw_list")
else
	injection_require_command "$INJECTION_IW"
	tmp_iw=$(mktemp)
	trap 'rm -f "$tmp_iw"' EXIT HUP INT TERM
	"$INJECTION_IW" list >"$tmp_iw"
	legal=$(injection_legal_frequencies "$tmp_iw")
fi
[ -n "$legal" ] || injection_die 'no enabled 2.4/5/6 GHz frequencies found'

if [ "$INJECTION_CHANNELS" = auto ]; then
	channels=
	for wanted_band in 2.4 5 6; do
		for freq in $legal; do
			if [ "$(injection_band_for_frequency "$freq")" = "$wanted_band" ]; then
				channels="${channels}${channels:+ }$freq"
				break
			fi
		done
	done
else
	channels=$INJECTION_CHANNELS
fi

for freq in $channels; do
	injection_band_for_frequency "$freq" >/dev/null
	injection_frequency_is_legal "$legal" "$freq" || \
		injection_die "frequency is not enabled by iw: $freq"
done

if [ "$mode" = live ]; then
	[ "$(id -u)" -eq 0 ] || injection_die 'live mode requires root'
	[ "${INJECTION_OWNED_LAB:-}" = YES ] || injection_die 'set INJECTION_OWNED_LAB=YES for live mode'
	[ "$INJECTION_RECEIVER" != 00:00:00:00:00:00 ] || injection_die 'set an owned receiver identity'
	for cmd in "$INJECTION_INJECT_CMD" "$INJECTION_CAPTURE_CMD" "$INJECTION_STATUS_CMD"; do
		injection_require_command "$cmd"
	done
	INJECTION_DRY_RUN=0
else
	INJECTION_DRY_RUN=1
fi
export INJECTION_DRY_RUN

printf 'contract_version=1\nworktree=%s\ncommit=%s\nmode=%s\nlegal=%s\nchannels=%s\n' \
	"$INJECTION_WORKTREE" "$target_commit" "$mode" "$legal" "$channels"
printf 'receiver_schema=tag\\tfrequency_mhz\\treceiver_ts\n'
printf 'status_schema=tag\\tstatus\\tstatus_ts\n'
printf 'acceptance=min_delivery_%s_percent,no_transition_leaks,no_stale_or_failed_status,no_kernel_warning_or_firmware_recovery\n' \
	"$INJECTION_MIN_DELIVERY"

run_id="$(date -u +%Y%m%dT%H%M%SZ)-$target_commit"
seq=0
for freq in $channels; do
	injection_run "$INJECTION_CAPTURE_CMD" "$freq" "receiver-$run_id-$freq.tsv"
done

for src in $channels; do
	for dst in $channels; do
		[ "$src" != "$dst" ] || continue
		seq=$((seq + 1))
		injection_run "$INJECTION_IW" dev "$INJECTION_IFACE" set freq "$src"
		tag=$(injection_tag "$run_id" pre "$src" "$dst" unicast "$seq")
		injection_run "$INJECTION_INJECT_CMD" "$INJECTION_IFACE" "$INJECTION_RECEIVER" \
			"$tag" unicast "$INJECTION_ROUNDS"
		seq=$((seq + 1))
		injection_run "$INJECTION_IW" dev "$INJECTION_IFACE" set freq "$dst"
		tag=$(injection_tag "$run_id" post "$src" "$dst" unicast "$seq")
		injection_run "$INJECTION_INJECT_CMD" "$INJECTION_IFACE" "$INJECTION_RECEIVER" \
			"$tag" unicast "$INJECTION_ROUNDS"
	done
done

injection_run "$INJECTION_STATUS_CMD" "status-$run_id.tsv"

printf 'matrix_counts=unicast:%s,broadcast:%s,transition_rounds:%s\n' \
	"$INJECTION_UNICAST_COUNT" "$INJECTION_BROADCAST_COUNT" "$INJECTION_ROUNDS"
