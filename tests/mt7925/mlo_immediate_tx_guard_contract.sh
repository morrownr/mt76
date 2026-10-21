#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Compile and execute actual subsystem/shared dispatch against literal
# invalid/retired/missing WCID outcomes. No source-location/count contract.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
exec python3 tests/mt7925/test_mlo_device_isolation.py \
  MloDeviceIsolation.test_immediate_inactive_sta \
  MloDeviceIsolation.test_immediate_invalid_sta \
  MloDeviceIsolation.test_immediate_no_live_sta \
  MloDeviceIsolation.test_immediate_inactive_bmc \
  MloDeviceIsolation.test_immediate_invalid_bmc \
  MloDeviceIsolation.test_immediate_no_live_bmc \
  MloDeviceIsolation.test_immediate_missing_sta \
  MloDeviceIsolation.test_immediate_missing_bmc \
  MloDeviceIsolation.test_immediate_missing_foreign \
  MloDeviceIsolation.test_immediate_missing_conf \
  MloDeviceIsolation.test_immediate_missing_peer
