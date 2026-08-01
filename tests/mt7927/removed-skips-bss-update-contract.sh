#!/bin/sh
set -eu

repo="${1:-.}"
mcu="$repo/mt7925/mcu.c"
connac="$repo/mt76_connac_mcu.c"

check_guard_before_send()
{
	file="$1"
	fn="$2"
	guard="$3"

	awk -v fn="$fn" -v guard="$guard" '
	  index($0, fn) { in_fn = 1 }
	  in_fn && index($0, guard) { seen_guard = NR }
	  in_fn && /MCU_UNI_CMD\(BSS_INFO_UPDATE\)/ {
	    seen_send = NR
	    if (!seen_guard)
	      bad = 1
	  }
	  in_fn && /^}/ {
	    if (!seen_guard || bad || seen_guard > seen_send)
	      exit 1
	    found = 1
	    exit 0
	  }
	  END {
	    if (!found)
	      exit 1
	  }
	' "$file"
}

check_guard_before_send "$mcu" "int mt7925_mcu_set_chctx(" "test_bit(MT76_REMOVED, &phy->state)"
check_guard_before_send "$mcu" "int mt7925_mcu_set_timing(" "test_bit(MT76_REMOVED, &phy->mt76->state)"
check_guard_before_send "$mcu" "int mt7925_mcu_add_bss_info_sta(" "test_bit(MT76_REMOVED, &phy->mt76->state)"
check_guard_before_send "$mcu" "mt7925_mcu_uni_bss_ps(" "test_bit(MT76_REMOVED, &dev->mphy.state)"
check_guard_before_send "$mcu" "mt7925_mcu_uni_bss_bcnft(" "test_bit(MT76_REMOVED, &dev->mphy.state)"
check_guard_before_send "$mcu" "mt7925_mcu_set_bss_pm(" "test_bit(MT76_REMOVED, &dev->mphy.state)"
check_guard_before_send "$mcu" "int mt7925_mcu_set_eht_pp(" "test_bit(MT76_REMOVED, &phy->state)"
check_guard_before_send "$mcu" "mt7925_mcu_uni_add_beacon_offload(" "test_bit(MT76_REMOVED, &dev->mphy.state)"
check_guard_before_send "$mcu" "void mt7925_mcu_del_dev(" "test_bit(MT76_REMOVED, &mdev->phy.state)"
check_guard_before_send "$connac" "int mt76_connac_mcu_uni_set_chctx(" "test_bit(MT76_REMOVED, &phy->state)"
check_guard_before_send "$connac" "int mt76_connac_mcu_uni_add_bss(" "test_bit(MT76_REMOVED, &phy->state)"
