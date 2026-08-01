#!/bin/sh

set -eu

CANDIDATE_TREE=/home/pi/wireless-next-mt7927/.worktrees/mt7927-monitor-rearm-v3-transaction
CANDIDATE_HEAD=ca013156cfc2e7641e273a5ea0f6ece896facd43
LOCALVERSION=-mt7927-monitor-v3
MODE=dry-run
BUILD_DIR=${BUILD_DIR:-/home/pi/mt7927-kernel-build}
JOBS=${JOBS:-2}

[ "${1:-}" != --execute ] || { MODE=execute; shift; }
[ "$#" -eq 0 ] || { echo 'usage: build-kernel.sh [--execute]' >&2; exit 2; }

running=$(uname -r)
config=/boot/config-$running
printf 'mode=%s\nsource=%s\nhead=%s\nconfig=%s\nbuild_dir=%s\nlocalversion=%s\n' \
	"$MODE" "$CANDIDATE_TREE" "$CANDIDATE_HEAD" "$config" "$BUILD_DIR" "$LOCALVERSION"
[ "$MODE" = execute ] || exit 0

[ "$(git -C "$CANDIDATE_TREE" rev-parse HEAD)" = "$CANDIDATE_HEAD" ] || { echo 'candidate head changed' >&2; exit 1; }
git -C "$CANDIDATE_TREE" diff --quiet HEAD || { echo 'candidate worktree is dirty' >&2; exit 1; }
[ -r "$config" ] || { echo "missing running-kernel config: $config" >&2; exit 1; }
mkdir -p "$BUILD_DIR"
cp "$config" "$BUILD_DIR/.config"
make -C "$CANDIDATE_TREE" O="$BUILD_DIR" LOCALVERSION="$LOCALVERSION" olddefconfig
make -C "$CANDIDATE_TREE" O="$BUILD_DIR" LOCALVERSION="$LOCALVERSION" -j"$JOBS" bindeb-pkg
