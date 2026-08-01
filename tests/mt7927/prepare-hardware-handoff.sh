#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
CANDIDATE_TREE=/home/pi/wireless-next-mt7927/.worktrees/mt7927-monitor-rearm-v3-transaction
CANDIDATE_BASE=4b02d205c
CANDIDATE_HEAD=ca013156cfc2e7641e273a5ea0f6ece896facd43
KERNEL_VERSION=7.2.0-rc1
OUTPUT=
MODE=prepare

usage()
{
	cat <<'EOF'
Usage: prepare-hardware-handoff.sh [--dry-run] [--output DIR]

Creates a non-activating hardware-reproduction bundle for the reviewed MT7927
wireless-next candidate. It does not build, install, unload, load, or reboot.
EOF
}

while [ "$#" -gt 0 ]; do
	case $1 in
		--dry-run) MODE=dry-run ;;
		--output) shift; [ "$#" -gt 0 ] || { usage >&2; exit 2; }; OUTPUT=$1 ;;
		--help|-h) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
	esac
	shift
done

[ -e "$CANDIDATE_TREE/.git" ] || { echo 'candidate worktree is missing' >&2; exit 1; }
actual_head=$(git -C "$CANDIDATE_TREE" rev-parse HEAD)
[ "$actual_head" = "$CANDIDATE_HEAD" ] || { echo 'candidate head changed' >&2; exit 1; }
git -C "$CANDIDATE_TREE" diff --quiet HEAD || { echo 'candidate worktree is dirty' >&2; exit 1; }
actual_version=$(make -s -C "$CANDIDATE_TREE" kernelversion)
[ "$actual_version" = "$KERNEL_VERSION" ] || { echo 'candidate kernel version changed' >&2; exit 1; }

printf 'mode=%s\ncandidate_tree=%s\ncandidate_base=%s\ncandidate_head=%s\nkernel_version=%s\n' \
	"$MODE" "$CANDIDATE_TREE" "$CANDIDATE_BASE" "$CANDIDATE_HEAD" "$KERNEL_VERSION"

[ "$MODE" = prepare ] || exit 0
[ -n "$OUTPUT" ] || { echo '--output is required unless --dry-run is used' >&2; exit 2; }
[ ! -e "$OUTPUT" ] || { echo 'output path already exists' >&2; exit 1; }

mkdir -p "$OUTPUT/patches" "$OUTPUT/tools"
git -C "$CANDIDATE_TREE" format-patch --quiet --output-directory "$OUTPUT/patches" \
	"$CANDIDATE_BASE..$CANDIDATE_HEAD"

for file in build-kernel.sh install-kernel.sh rollback-kernel.sh run-hardware-matrix.sh; do
	cp "$SCRIPT_DIR/handoff/$file" "$OUTPUT/$file"
	chmod 0755 "$OUTPUT/$file"
done
cp "$SCRIPT_DIR/handoff/README.md" "$OUTPUT/README.md"
for file in monitor-contract.sh lib-monitor-common.sh injection-matrix.sh \
	lib-injection-contract.sh injection-contract.sh; do
	cp "$SCRIPT_DIR/$file" "$OUTPUT/tools/$file"
	chmod 0755 "$OUTPUT/tools/$file"
done

cat >"$OUTPUT/MANIFEST" <<EOF
candidate_tree=$CANDIDATE_TREE
candidate_base=$CANDIDATE_BASE
candidate_head=$CANDIDATE_HEAD
kernel_version=$KERNEL_VERSION
localversion=-mt7927-monitor-v3
activation=manual-only
hardware_success=unverified
mlo_str_tri_band=unverified
EOF

printf 'output=%s\n' "$OUTPUT"
