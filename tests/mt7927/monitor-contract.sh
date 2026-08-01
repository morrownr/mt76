#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=tests/mt7927/lib-monitor-common.sh
. "$SCRIPT_DIR/lib-monitor-common.sh"

EXPECTED_TREE=/home/pi/wireless-next-mt7927/.worktrees/mt7927-monitor-rearm-v3-transaction
EXPECTED_HEAD=ca013156cfc2e7641e273a5ea0f6ece896facd43
MONITOR_IFACE=mt7927mon
MONITOR_PHY=phy0
MONITOR_BANDS=2.4,5,6,5,2.4
MONITOR_ROUNDS=1
MONITOR_MODE=dry-run
MONITOR_SINGLE_BAND=
MONITOR_SINGLE_FREQ=
MONITOR_ALL_ENABLED=0
MONITOR_IW_LIST=
MONITOR_FREQUENCIES=

usage()
{
	cat <<'EOF'
Usage: monitor-contract.sh [options]

Defaults to a non-disruptive dry-run command plan for an owned lab.

  --interface NAME       monitor interface (default: mt7927mon)
  --phy PHY              wiphy name used for interface recreation (default: phy0)
  --transitions LIST     comma-separated bands (default: 2.4,5,6,5,2.4)
  --band BAND            select one of 2.4, 5, or 6
  --frequency MHZ        override frequency for --band
  --all-enabled          visit every enabled 2.4/5/6 GHz frequency reported
                         by cfg80211; includes passive/DFS monitor channels
  --iw-list FILE         use captured iw phy/list output for --all-enabled
  --rounds COUNT         transition rounds (default: 1)
  --worktree PATH        must equal the pinned worktree
  --execute              execute the plan; requires root and
                         MT7927_MONITOR_OWNED_LAB=yes
  --dry-run              print commands only (default)
  --help                 show this help
EOF
}

while [ "$#" -gt 0 ]; do
	case $1 in
		--interface) [ "$#" -ge 2 ] || monitor_die '--interface needs a value'; MONITOR_IFACE=$2; shift 2 ;;
		--phy) [ "$#" -ge 2 ] || monitor_die '--phy needs a value'; MONITOR_PHY=$2; shift 2 ;;
		--transitions) [ "$#" -ge 2 ] || monitor_die '--transitions needs a value'; MONITOR_BANDS=$2; shift 2 ;;
		--band) [ "$#" -ge 2 ] || monitor_die '--band needs a value'; MONITOR_SINGLE_BAND=$2; MONITOR_BANDS=$2; shift 2 ;;
		--frequency) [ "$#" -ge 2 ] || monitor_die '--frequency needs a value'; MONITOR_SINGLE_FREQ=$2; shift 2 ;;
		--all-enabled) MONITOR_ALL_ENABLED=1; shift ;;
		--iw-list) [ "$#" -ge 2 ] || monitor_die '--iw-list needs a value'; MONITOR_IW_LIST=$2; shift 2 ;;
		--rounds) [ "$#" -ge 2 ] || monitor_die '--rounds needs a value'; MONITOR_ROUNDS=$2; shift 2 ;;
		--worktree) [ "$#" -ge 2 ] || monitor_die '--worktree needs a value'; [ "$2" = "$EXPECTED_TREE" ] || monitor_die 'worktree does not match pinned target'; shift 2 ;;
		--execute) MONITOR_MODE=execute; shift ;;
		--dry-run) MONITOR_MODE=dry-run; shift ;;
		--help|-h) usage; exit 0 ;;
		*) monitor_die "unknown argument: $1" ;;
	esac
done

monitor_validate_iface "$MONITOR_IFACE"
monitor_validate_iface "$MONITOR_PHY"
monitor_validate_uint rounds "$MONITOR_ROUNDS"
monitor_validate_bands "$MONITOR_BANDS"
if [ -n "$MONITOR_SINGLE_FREQ" ]; then
	[ -n "$MONITOR_SINGLE_BAND" ] || monitor_die '--frequency requires --band'
	monitor_validate_frequency "$MONITOR_SINGLE_BAND" "$MONITOR_SINGLE_FREQ"
elif [ -n "$MONITOR_SINGLE_BAND" ]; then
	MONITOR_SINGLE_FREQ=$(monitor_default_frequency "$MONITOR_SINGLE_BAND")
fi

tmp_iw=
if [ "$MONITOR_ALL_ENABLED" -eq 1 ]; then
	if [ -z "$MONITOR_IW_LIST" ]; then
		[ "$MONITOR_MODE" = execute ] || monitor_die '--all-enabled dry-run requires --iw-list'
		command -v iw >/dev/null 2>&1 || monitor_die 'iw is required'
		tmp_iw=$(mktemp)
		trap 'rm -f "$tmp_iw"' EXIT HUP INT TERM
		iw phy "$MONITOR_PHY" info >"$tmp_iw"
		MONITOR_IW_LIST=$tmp_iw
	fi
	[ -r "$MONITOR_IW_LIST" ] || monitor_die 'iw-list file is not readable'
	MONITOR_FREQUENCIES=$(monitor_enabled_frequencies "$MONITOR_IW_LIST")
	[ -n "$MONITOR_FREQUENCIES" ] || monitor_die 'no enabled 2.4/5/6 GHz frequencies found'
fi

if [ "$MONITOR_MODE" = execute ]; then
	[ "${MT7927_MONITOR_OWNED_LAB:-}" = yes ] || monitor_die 'execute requires MT7927_MONITOR_OWNED_LAB=yes'
	[ "$(id -u)" -eq 0 ] || monitor_die 'execute requires root'
	[ -e "$EXPECTED_TREE/.git" ] || monitor_die 'pinned worktree is unavailable'
	[ "$(git -C "$EXPECTED_TREE" rev-parse HEAD)" = "$EXPECTED_HEAD" ] || monitor_die 'pinned worktree head changed'
	git -C "$EXPECTED_TREE" diff --quiet HEAD || monitor_die 'pinned worktree is dirty'
	command -v iw >/dev/null 2>&1 || monitor_die 'iw is required'
	command -v ip >/dev/null 2>&1 || monitor_die 'ip is required'
fi

printf 'mode=%s\nworktree=%s\n' "$MONITOR_MODE" "$EXPECTED_TREE"
printf 'head=%s\n' "$(git -C "$EXPECTED_TREE" rev-parse HEAD 2>/dev/null || printf unavailable)"
printf 'interface=%s phy=%s rounds=%s bands=%s\n' \
	"$MONITOR_IFACE" "$MONITOR_PHY" "$MONITOR_ROUNDS" "$MONITOR_BANDS"
if [ "$MONITOR_ALL_ENABLED" -eq 1 ]; then
	printf 'frequencies=%s\n' "$MONITOR_FREQUENCIES"
fi

monitor_prepare_interface

round=1
while [ "$round" -le "$MONITOR_ROUNDS" ]; do
	monitor_plan_round "$round"
	round=$((round + 1))
done
