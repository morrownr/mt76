#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Compile and execute actual subsystem/shared dispatch against literal
# invalid/retired/missing WCID outcomes. No source-location/count contract.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
exec python3 tests/mt7925/test_mlo_device_isolation.py \
  MloDeviceIsolation.test_bulk_invalid_forced \
  MloDeviceIsolation.test_bulk_invalid_tid \
  MloDeviceIsolation.test_bulk_invalid_control_port \
  MloDeviceIsolation.test_generic_mt7927
