#!/bin/sh
set -eu

repo="${1:-.}"
file="$repo/mt7925/pci.c"

awk '
	/static void mt7925e_unregister_device\(struct mt792x_dev \*dev\)/ { in_fn = 1 }
	in_fn && /__mt792x_mcu_drv_pmctrl\(dev\);/ {
		if (!drv_own_line) drv_own_line = NR
	}
	in_fn && /mt76_unregister_device\(&dev->mt76\);/ {
		unregister_line = NR
	}
	in_fn && /^}/ {
		if (!drv_own_line) {
			print "FAIL: unregister does not take driver ownership"
			exit 1
		}
		if (!unregister_line) {
			print "FAIL: unregister does not call mt76_unregister_device"
			exit 1
		}
		if (drv_own_line > unregister_line) {
			print "FAIL: driver ownership is taken after mac80211 unregister"
			found = 1
			exit 1
		}
		found = 1
		exit 0
	}
	END {
		if (!found) {
			print "FAIL: mt7925e_unregister_device contract was not checked"
			exit 1
		}
	}
' "$file"

echo "PASS unregister takes driver ownership before mac80211 unregister"
