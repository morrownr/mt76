#!/bin/sh

set -eu

MODE=dry-run
[ "${1:-}" != --execute ] || { MODE=execute; shift; }
[ "$#" -gt 0 ] || { echo 'usage: rollback-kernel.sh [--execute] PACKAGE...' >&2; exit 2; }

printf 'mode=%s\nremove=' "$MODE"
printf ' %s' "$@"
printf '\nreboot=manual-only\n'
[ "$MODE" = execute ] || exit 0
[ "$(id -u)" -eq 0 ] || { echo 'rollback requires root' >&2; exit 1; }
case $(uname -r) in
	*mt7927-monitor-v3*)
		echo 'Boot the previous distro kernel before removing the test kernel.' >&2
		exit 1
		;;
esac
dpkg --purge "$@"
