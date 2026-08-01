#!/bin/sh

set -eu

TEST_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
PREPARE="$TEST_DIR/prepare-hardware-handoff.sh"
EXPECTED_HEAD=ca013156cfc2e7641e273a5ea0f6ece896facd43

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

pass=0

ok()
{
	pass=$((pass + 1))
	printf 'ok %s - %s\n' "$pass" "$1"
}

assert_file()
{
	[ -f "$1" ] || {
		printf 'not ok - missing file: %s\n' "$1" >&2
		exit 1
	}
	ok "$2"
}

assert_contains()
{
	grep -F -- "$2" "$1" >/dev/null || {
		printf 'not ok - %s lacks: %s\n' "$1" "$2" >&2
		exit 1
	}
	ok "$3"
}

make_test_deb()
{
	pkg=$1
	version=$2
	out=$3
	dir=$tmp/deb-$pkg-$version

	mkdir -p "$dir/DEBIAN"
	cat >"$dir/DEBIAN/control" <<EOF
Package: $pkg
Version: $version
Architecture: amd64
Maintainer: MT7927 selftest <nobody@example.invalid>
Description: MT7927 handoff installer selftest fixture
EOF
	dpkg-deb --build "$dir" "$out" >/dev/null
}

"$PREPARE" --help >"$tmp/help"
ok 'help is non-destructive'

"$PREPARE" --dry-run >"$tmp/dry-run"
assert_contains "$tmp/dry-run" "candidate_head=$EXPECTED_HEAD" \
	'dry-run pins the reviewed commit'
assert_contains "$tmp/dry-run" 'kernel_version=7.2.0-rc1' \
	'dry-run identifies the candidate kernel'

"$PREPARE" --output "$tmp/handoff"
assert_file "$tmp/handoff/MANIFEST" 'manifest is generated'
assert_file "$tmp/handoff/build-kernel.sh" 'build script is generated'
assert_file "$tmp/handoff/install-kernel.sh" 'install script is generated'
assert_file "$tmp/handoff/rollback-kernel.sh" 'rollback script is generated'
assert_file "$tmp/handoff/run-hardware-matrix.sh" \
	'hardware matrix script is generated'
assert_file "$tmp/handoff/README.md" 'user-run procedure is generated'
assert_contains "$tmp/handoff/MANIFEST" "candidate_head=$EXPECTED_HEAD" \
	'manifest pins the reviewed commit'
assert_contains "$tmp/handoff/install-kernel.sh" '--execute' \
	'installation requires explicit execution'
assert_contains "$tmp/handoff/install-kernel.sh" 'reboot is never automatic' \
	'installation cannot reboot automatically'
assert_contains "$tmp/handoff/rollback-kernel.sh" '--execute' \
	'rollback requires explicit execution'
assert_contains "$tmp/handoff/run-hardware-matrix.sh" '--all-enabled' \
	'monitor matrix covers all enabled channels dynamically'
assert_contains "$tmp/handoff/run-hardware-matrix.sh" '/usr/sbin' \
	'hardware matrix can find distro wireless tools in sbin'
assert_contains "$tmp/handoff/README.md" '2.4 -> 5 -> 6 -> 5 -> 2.4 GHz' \
	'user-run procedure includes the transition sequence'
assert_contains "$tmp/handoff/README.md" 'continuously increasing packet counts' \
	'user-run procedure requires live capture evidence'
assert_contains "$tmp/handoff/README.md" 'receiver-side' \
	'user-run procedure requires receiver-side TX evidence'
assert_contains "$tmp/handoff/README.md" 'MLO/STR and simultaneous tri-band aggregation remain unverified' \
	'user-run procedure preserves the MLO scope boundary'

"$tmp/handoff/build-kernel.sh" >"$tmp/build-dry-run"
assert_contains "$tmp/build-dry-run" 'mode=dry-run' \
	'generated kernel build defaults to dry-run'
mkdir -p "$tmp/handoff/packages"
make_test_deb linux-image-7.2.0-rc1-mt7927-monitor-v3 \
	7.2.0~rc1-gb94bdfa1d99d-3 \
	"$tmp/handoff/packages/linux-image-old.deb"
make_test_deb linux-image-7.2.0-rc1-mt7927-monitor-v3-dbg \
	7.2.0~rc1-gca013156cfc2-4 \
	"$tmp/handoff/packages/linux-image-debug.deb"
make_test_deb linux-image-7.2.0-rc1-mt7927-monitor-v3 \
	7.2.0~rc1-gca013156cfc2-4 \
	"$tmp/handoff/packages/linux-image-candidate.deb"
make_test_deb linux-headers-7.2.0-rc1-mt7927-monitor-v3 \
	7.2.0~rc1-gb94bdfa1d99d-3 \
	"$tmp/handoff/packages/linux-headers-old.deb"
make_test_deb linux-headers-7.2.0-rc1-mt7927-monitor-v3 \
	7.2.0~rc1-gca013156cfc2-4 \
	"$tmp/handoff/packages/linux-headers-candidate.deb"
PACKAGE_DIR="$tmp/handoff/packages" "$tmp/handoff/install-kernel.sh" \
	>"$tmp/install-dry-run"
assert_contains "$tmp/install-dry-run" 'reboot=manual-only' \
	'generated install never reboots automatically'
assert_contains "$tmp/install-dry-run" 'linux-image-candidate.deb' \
	'generated install selects the pinned candidate image'
assert_contains "$tmp/install-dry-run" 'linux-headers-candidate.deb' \
	'generated install selects the pinned candidate headers'
if grep -E 'linux-(image|headers)-(old|debug)\.deb' \
	"$tmp/install-dry-run" >/dev/null; then
	printf 'not ok - generated install selected stale or debug packages\n' >&2
	exit 1
fi
ok 'generated install excludes stale and debug packages'
"$tmp/handoff/rollback-kernel.sh" linux-image-7.2.0-rc1-mt7927-monitor-v3 \
	>"$tmp/rollback-dry-run"
assert_contains "$tmp/rollback-dry-run" 'mode=dry-run' \
	'generated rollback defaults to dry-run'

cat >"$tmp/iw-list" <<'EOF'
Band 1:
	Frequencies:
		* 2412 MHz [1] (20.0 dBm)
Band 2:
	Frequencies:
		* 5180 MHz [36] (23.0 dBm)
Band 4:
	Frequencies:
		* 5955 MHz [1] (23.0 dBm)
EOF
INJECTION_RECEIVER=02:00:00:00:00:01 \
	"$tmp/handoff/run-hardware-matrix.sh" --iw-list "$tmp/iw-list" \
	>"$tmp/matrix-dry-run"
assert_contains "$tmp/matrix-dry-run" "head=$EXPECTED_HEAD" \
	'generated matrix pins the reviewed candidate'

if find "$tmp/handoff" -type f -name '*.ko*' | grep . >/dev/null; then
	printf 'not ok - handoff unexpectedly contains a prebuilt module\n' >&2
	exit 1
fi
ok 'handoff contains no unverified prebuilt module'

printf '1..%s\n' "$pass"
