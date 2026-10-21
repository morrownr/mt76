#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -euo pipefail

base=${MLO_PROTECTED_BASE:-a031e8e6722ec461df887f9894281e55a8c8af82}
root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'MLO isolation contract failed: %s\n' "$1" >&2
	exit 1
}

for path in \
	mt7925/mlo/mlo.c \
	mt7925/mlo/ttlm.c \
	mt7925/mlo/mlo.h \
	mt7925/mlo/internal.h \
	mt7925/mlo/policy.h
do
	test -f "$path" || fail "missing subsystem file $path"
done

test ! -e mt7925/ttlm.h || fail "legacy root TTLM implementation remains"

baseline_main=$(git show "$base:mt7925/main.c")
# Architecture now routes association/radio/TX through mt7925/mlo. Whole-file
# immutability would reject that approved boundary (and even artifact copies
# named mcu.c). Protect the actual monitor leaf bodies and execute the existing
# device/channel/TX effect tests below instead.

extract_function()
{
	local name=$1
	awk -v name="$name" '
		$0 ~ name "\\(" { found = 1 }
		found {
			print
			line = $0
			opens = gsub(/\{/, "{", line)
			closes = gsub(/\}/, "}", line)
			depth += opens - closes
			if (opens)
				body = 1
			if (body && depth == 0)
				exit
		}
	'
}

protected_functions=(
	mt7925_monitor_update_chan
	mt7925_monitor_arm_sniffer
	mt7925_sniffer_interface_iter
)

for function in "${protected_functions[@]}"
do
	rg -q "$function\\(" <<< "$baseline_main" ||
		fail "baseline function not found: $function"
	rg -q "$function\\(" mt7925/main.c ||
		fail "current function not found: $function"
	diff -u \
		<(extract_function "$function" <<< "$baseline_main") \
		<(extract_function "$function" < mt7925/main.c) >/dev/null ||
		fail "protected function changed: $function"
done

dispatcher_defs=$(rg -n '^mt7925_mlo_dispatch\(' mt7925/mlo --glob '*.[ch]' | wc -l)
test "$dispatcher_defs" -eq 1 ||
	fail "expected exactly one dispatcher definition, found $dispatcher_defs"

wrapper_calls=$(rg -n 'mt7925_mlo_dispatch\([^,]+, &request\)' \
	mt7925/mlo/mlo.c | wc -l)
test "$wrapper_calls" -eq 2 ||
	fail "expected two public wrapper calls, found $wrapper_calls"

rg -q '^[[:space:]]*\.can_neg_ttlm = mt7925_mlo_can_neg_ttlm,' mt7925/main.c ||
	fail "main.c lacks the minimal TTLM registration hook"
rg -q 'mt7925_init_mlo_caps\(&dev->phy\)' mt7925/init.c ||
	fail "init.c lacks the subsystem capability hook"
rg -q 'mt7925_mlo_update_caps\(phy,' mt7925/mlo/mlo.c ||
	fail "subsystem lacks capability policy hook"

if rg -n 'mt7925_mlo_dispatch|mt7925_mlo_ttlm_validate' mt7925/main.c; then
	fail "main.c contains dispatcher or feature implementation logic"
fi

mlo_consumers=$(rg -l \
	'mt7925_mlo_(update_caps|can_neg_ttlm|dispatch|ttlm_)' \
	mt7925 --glob '*.[ch]' | sort)
while IFS= read -r path
do
	test -n "$path" || continue
	case "$path" in
	mt7925/main.c|mt7925/mlo/*)
		;;
	*)
		fail "experimental MLO symbol used outside the registration boundary: $path"
		;;
	esac
done <<< "$mlo_consumers"

# Wire command definitions/serialization remain MCU-owned; association and
# radio policy may call their public APIs. The former ban on identifiers in
# comments was unrelated to monitor isolation.
if rg -n '^struct (mt7925_mcu_|mt76_connac_).*\{' mt7925/mlo --glob '*.[ch]'; then
	fail "firmware wire structure defined inside policy subsystem"
fi

for fixture in test_mlo_device_isolation.py test_mlo_chanctx_routing.py \
	test_mlo_native_tx_headers.py test_mlo_radio_execution.py
do
	python3 "tests/mt7925/$fixture" || fail "effect fixture failed: $fixture"
done

printf 'MLO monitor/injection isolation contract passed\n'
