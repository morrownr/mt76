#!/bin/sh
set -eu

repo="${1:-.}"
file="$repo/mt7925/init.c"

awk '
	/static ssize_t mt7925_thermal_temp_show\(struct device \*dev,/ { in_fn = 1 }
	in_fn && /test_bit\(MT76_REMOVED, &mdev->mphy\.state\)/ { removed_guard = NR }
	in_fn && /mt7925_mcu_get_temperature\(phy\)/ { thermal_query = NR }
	in_fn && /^}/ {
		if (!removed_guard) {
			print "FAIL: thermal show does not check MT76_REMOVED"
			exit 1
		}
		if (!thermal_query) {
			print "FAIL: thermal show does not query MCU temperature"
			exit 1
		}
		if (removed_guard > thermal_query) {
			print "FAIL: MT76_REMOVED guard appears after MCU thermal query"
			exit 1
		}
		found = 1
		exit 0
	}
	END {
		if (!found) {
			print "FAIL: mt7925_thermal_temp_show contract was not checked"
			exit 1
		}
	}
' "$file"

echo "PASS thermal show avoids MCU query after MT76_REMOVED"
