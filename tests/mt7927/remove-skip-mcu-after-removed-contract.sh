#!/bin/sh
set -eu

repo="${1:-.}"
file="$repo/mt792x_core.c"

awk '
	/void mt792x_mac_link_bss_remove\(struct mt792x_dev \*dev,/ { in_fn = 1 }
	in_fn && /!test_bit\(MT76_REMOVED, &dev->mphy\.state\)/ { removed_guard = NR }
	in_fn && /mt76_connac_mcu_uni_add_dev\(&dev->mphy, link_conf, &mconf->mt76,/ { mcu_delete = NR }
	in_fn && /rcu_assign_pointer\(dev->mt76\.wcid\[idx\], NULL\);/ { host_cleanup = NR }
	in_fn && /^}/ {
		if (!removed_guard) {
			print "FAIL: BSS remove does not check MT76_REMOVED"
			exit 1
		}
		if (!mcu_delete) {
			print "FAIL: BSS remove does not contain MCU delete path"
			exit 1
		}
		if (!host_cleanup) {
			print "FAIL: BSS remove does not keep host cleanup path"
			exit 1
		}
		if (removed_guard > mcu_delete) {
			print "FAIL: MT76_REMOVED guard appears after MCU delete"
			exit 1
		}
		if (host_cleanup < mcu_delete) {
			print "FAIL: host cleanup unexpectedly moved before MCU delete"
			exit 1
		}
		found = 1
		exit 0
	}
	END {
		if (!found) {
			print "FAIL: mt792x_mac_link_bss_remove contract was not checked"
			exit 1
		}
	}
' "$file"

echo "PASS BSS remove skips MCU delete after MT76_REMOVED while keeping host cleanup"
