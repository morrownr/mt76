#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# After a pair transition the firmware keeps reporting the retired link's WLAN_IDX for unicast frames received on the
# incoming link (measured: 100 % of 682/270/137/74/22 frames in five runs, all decrypted). mac80211 derives the RX link
# from the WCID, so the frames land on an inactive link. mt7925_mac_fill_rx() must be able to re-attribute a unicast
# frame by the channel it was received on, through the pure resolver, behind a default-off knob.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

# Execute the owner functions against literal topology outcomes, including
# stale BSS/WCID bands, retired/ambiguous members, frequency remaps and NULL.
exec python3 tests/mt7925/test_rx_link_api.py \
  RxLinkApiTest.test_native_rx_resolver_checks_bss_band_before_accepting_raw_wcid
