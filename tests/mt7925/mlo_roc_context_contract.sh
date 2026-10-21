#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Execute the actual subsystem policy against literal MT7927 and generic
# request fixtures, including ACQUIRE/SUB_LINK, A/G and diagnostic overrides.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
python3 tests/mt7925/test_mlo_radio_execution.py \
	RadioExecutionTest.test_slot_policy_preserves_mt7927_and_generic_wire_choices
