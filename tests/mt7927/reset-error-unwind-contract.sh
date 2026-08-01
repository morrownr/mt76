#!/bin/sh
set -eu

repo="${1:-.}"
file="$repo/mt7925/pci_mac.c"

awk '
	/int mt7925e_mac_reset\(struct mt792x_dev \*dev\)/ { in_fn = 1 }
	in_fn && /mt792xe_mcu_fw_pmctrl\(dev\);/ { seen = 1; next }
	seen && /if \(err\)/ { in_err = 1; next }
	in_err && /return err;/ {
		print "FAIL: mt792xe_mcu_fw_pmctrl() error returns before reset unwind"
		exit 1
	}
	in_err && /goto out;/ {
		found = 1
		exit 0
	}
	END {
		if (!found) {
			print "FAIL: mt792xe_mcu_fw_pmctrl() error does not unwind through out"
			exit 1
		}
	}
' "$file"

echo "PASS reset fw_pmctrl errors unwind through out"
