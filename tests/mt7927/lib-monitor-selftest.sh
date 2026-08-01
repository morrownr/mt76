#!/bin/sh

set -eu

TEST_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
CONTRACT="$TEST_DIR/monitor-contract.sh"
EXPECTED_TREE=/home/pi/wireless-next-mt7927/.worktrees/mt7927-monitor-rearm-v3-transaction
EXPECTED_HEAD=ca013156cfc2e7641e273a5ea0f6ece896facd43

pass=0
fail=0

ok()
{
	pass=$((pass + 1))
	printf 'ok %d - %s\n' "$pass" "$1"
}

not_ok()
{
	fail=$((fail + 1))
	printf 'not ok %d - %s\n' "$((pass + fail))" "$1" >&2
}

expect_success()
{
	name=$1
	shift
	if "$@" >"$tmp/out" 2>"$tmp/err"; then
		ok "$name"
	else
		not_ok "$name"
		sed 's/^/  /' "$tmp/err" >&2
	fi
}

expect_failure()
{
	name=$1
	shift
	if "$@" >"$tmp/out" 2>"$tmp/err"; then
		not_ok "$name"
	else
		ok "$name"
	fi
}

expect_output()
{
	name=$1
	pattern=$2
	shift 2
	if "$@" >"$tmp/out" 2>"$tmp/err" && grep -F -- "$pattern" "$tmp/out" >/dev/null; then
		ok "$name"
	else
		not_ok "$name"
		cat "$tmp/out" "$tmp/err" | sed 's/^/  /' >&2
	fi
}

expect_before()
{
	name=$1
	first=$2
	second=$3
	shift 3
	if "$@" >"$tmp/out" 2>"$tmp/err"; then
		first_line=$(grep -nF -- "$first" "$tmp/out" | head -n 1 | cut -d: -f1 || true)
		second_line=$(grep -nF -- "$second" "$tmp/out" | head -n 1 | cut -d: -f1 || true)
		if [ -n "$first_line" ] && [ -n "$second_line" ] &&
		    [ "$first_line" -lt "$second_line" ]; then
			ok "$name"
			return
		fi
	fi
	not_ok "$name"
	cat "$tmp/out" "$tmp/err" | sed 's/^/  /' >&2
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cat >"$tmp/iw-list" <<'EOF'
Band 1:
	Frequencies:
		* 2412.0 MHz [1] (20.0 dBm)
		* 2437.0 MHz [6] (20.0 dBm)
		* 2467.0 MHz [12] (disabled)
Band 2:
	Frequencies:
		* 5180.0 MHz [36] (23.0 dBm)
		* 5260.0 MHz [52] (radar detection)
Band 4:
	Frequencies:
		* 5955.0 MHz [1] (23.0 dBm) (no IR)
		* 5975.0 MHz [5] (disabled)
EOF

expect_success 'help does not require a radio' "$CONTRACT" --help
expect_failure 'interface names reject shell metacharacters' \
	"$CONTRACT" --interface 'mon0;reboot'
expect_failure '2.4 GHz rejects an out-of-band frequency' \
	"$CONTRACT" --band 2.4 --frequency 5180
expect_failure '5 GHz rejects an out-of-band frequency' \
	"$CONTRACT" --band 5 --frequency 5955
expect_failure '6 GHz rejects an out-of-band frequency' \
	"$CONTRACT" --band 6 --frequency 5180
expect_failure 'transition grammar rejects unknown bands' \
	"$CONTRACT" --transitions '2.4,7,5'
expect_failure 'rounds must be positive integers' \
	"$CONTRACT" --rounds 0
expect_failure 'execute requires owned-lab acknowledgement' \
	env MT7927_MONITOR_OWNED_LAB= "$CONTRACT" --execute
expect_output 'default mode is dry-run' 'mode=dry-run' "$CONTRACT"
expect_output 'contract pins the requested worktree' "worktree=$EXPECTED_TREE" \
	"$CONTRACT"
expect_output 'contract pins the reviewed candidate head' "head=$EXPECTED_HEAD" \
	"$CONTRACT"
expect_output 'default matrix covers 2.4 GHz' 'set freq 2412' "$CONTRACT"
expect_output 'default matrix covers 5 GHz' 'set freq 5180' "$CONTRACT"
expect_output 'default matrix covers 6 GHz' 'set freq 5955' "$CONTRACT"
expect_before 'plan creates monitor interface before first use' \
	'iw phy phy0 interface add mt7927mon type monitor' \
	'ip link set dev mt7927mon down' "$CONTRACT"
expect_output 'plan covers interface recreation' 'transition=interface-recreate' \
	"$CONTRACT"
expect_output 'plan covers chanctx remove and re-add' 'transition=chanctx-readd' \
	"$CONTRACT"
expect_output 'custom transition order is preserved' 'bands=6,5,2.4' \
	"$CONTRACT" --transitions '6,5,2.4'
expect_output 'all-channel plan follows enabled regulatory frequencies' \
	'frequencies=2412,2437,5180,5260,5955' "$CONTRACT" --all-enabled \
	--iw-list "$tmp/iw-list"
expect_output 'all-channel plan includes DFS frequencies for passive monitor' \
	'set freq 5260' "$CONTRACT" --all-enabled --iw-list "$tmp/iw-list"

printf '1..%d\n' "$((pass + fail))"
printf '# pass=%d fail=%d\n' "$pass" "$fail"
test "$fail" -eq 0
