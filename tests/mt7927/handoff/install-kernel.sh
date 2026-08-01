#!/bin/sh

set -eu

# The reboot is never automatic. Installation only stages a separate kernel.
MODE=dry-run
PACKAGE_DIR=${PACKAGE_DIR:-$(pwd)}
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

[ "${1:-}" != --execute ] || { MODE=execute; shift; }
[ "$#" -eq 0 ] || { echo 'usage: install-kernel.sh [--execute]' >&2; exit 2; }

[ -r "$HERE/MANIFEST" ] || { echo 'handoff MANIFEST is missing' >&2; exit 1; }
candidate_head=$(sed -n 's/^candidate_head=//p' "$HERE/MANIFEST")
kernel_version=$(sed -n 's/^kernel_version=//p' "$HERE/MANIFEST")
localversion=$(sed -n 's/^localversion=//p' "$HERE/MANIFEST")
if [ -z "$candidate_head" ] || [ -z "$kernel_version" ] || \
	[ -z "$localversion" ]; then
	echo 'handoff MANIFEST is incomplete' >&2
	exit 1
fi
candidate_tag=$(printf '%s' "$candidate_head" | cut -c1-12)
image_package=linux-image-$kernel_version$localversion
headers_package=linux-headers-$kernel_version$localversion
image=
headers=

for deb in "$PACKAGE_DIR"/*.deb; do
	[ -e "$deb" ] || continue
	package=$(dpkg-deb -f "$deb" Package 2>/dev/null) || continue
	version=$(dpkg-deb -f "$deb" Version 2>/dev/null) || continue
	case $version in
		*g"$candidate_tag"*) ;;
		*) continue ;;
	esac
	case $package in
		"$image_package")
			[ -z "$image" ] || { echo 'duplicate candidate image packages' >&2; exit 1; }
			image=$deb
			;;
		"$headers_package")
			[ -z "$headers" ] || { echo 'duplicate candidate header packages' >&2; exit 1; }
			headers=$deb
			;;
	esac
done

[ -n "$image" ] || { echo "candidate image package not found in $PACKAGE_DIR" >&2; exit 1; }
[ -n "$headers" ] || { echo "candidate header package not found in $PACKAGE_DIR" >&2; exit 1; }
set -- "$image" "$headers"
printf 'mode=%s\nprevious_kernel=%s\npackages=' "$MODE" "$(uname -r)"
printf ' %s' "$@"
printf '\nreboot=manual-only\n'
[ "$MODE" = execute ] || exit 0
[ "$(id -u)" -eq 0 ] || { echo 'installation requires root' >&2; exit 1; }
dpkg -i "$@"
printf 'Installed without reboot. Select the new kernel manually for the test.\n'
