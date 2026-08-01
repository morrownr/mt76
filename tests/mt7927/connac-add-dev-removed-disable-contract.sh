#!/bin/sh
set -eu

repo="${1:-.}"
file="$repo/mt76_connac_mcu.c"

awk '
  /int mt76_connac_mcu_uni_add_dev\(struct mt76_phy \*phy,/ { in_fn = 1 }
  in_fn && /mt76_mcu_send_msg\(dev, cmd, data, len, true\)/ {
    first_send = NR
    if (!guard)
      bad = 1
  }
  in_fn && /if \(!enable && test_bit\(MT76_REMOVED, &phy->state\)\)/ {
    guard = NR
  }
  in_fn && /return 0;/ && guard && !guard_return {
    guard_return = NR
  }
  in_fn && /^}/ {
    if (!guard || !guard_return || bad || guard_return > first_send)
      exit 1
    found = 1
    exit 0
  }
  END {
    if (!found)
      exit 1
  }
' "$file"
