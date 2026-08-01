#!/bin/sh
set -eu

root=${1:?usage: mlo-capability-contract.sh path/to/mt76}

fail()
{
	printf 'FAIL: %s\n' "$1" >&2
	exit 1
}

grep -q 'u64 fw_chip_cap;' "$root/mt792x.h" ||
	fail "raw firmware chip capability is not preserved"

parse=$(sed -n '/mt7925_mcu_get_nic_capability(/,/^}/p' "$root/mt7925/mcu.c")
printf '%s\n' "$parse" | grep -q 'fw_chip_cap = dev->phy.chip_cap' ||
	fail "capability parser does not snapshot raw firmware value"

debug=$(sed -n '/mt7925_mlo_caps(/,/^}/p' "$root/mt7925/debugfs.c")
for field in fw_chip_cap chip_cap eml_cap; do
	printf '%s\n' "$debug" | grep -q "$field" ||
		fail "debugfs MLO report is missing $field"
done

grep -q '"mlo_caps"' "$root/mt7925/debugfs.c" ||
	fail "mlo_caps debugfs file is not registered"

printf 'PASS: MT7927 MLO capability contract\n'
