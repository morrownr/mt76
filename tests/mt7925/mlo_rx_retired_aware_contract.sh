#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# The MT7927 firmware names the MLD's first WCID in the RX descriptor for frames of every link; the host re-attributes
# by BAND_IDX. After a link replacement the retired WCID still matches the band of its replacement and the candidates
# carry stale WCID bands, so frames of the incoming link stay on the retired link (100 % of five runs). The band
# attribution must be able to treat a retired resolved WCID as a mismatch, exclude retired links from the candidates and
# take candidate bands from the link's BSS, behind a default-off knob.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

# Execute the owner functions against literal topology outcomes, including
# stale BSS/WCID bands, retired/ambiguous members, frequency remaps and NULL.
exec python3 tests/mt7925/test_rx_link_api.py \
  RxLinkApiTest.test_native_rx_resolver_checks_bss_band_before_accepting_raw_wcid
