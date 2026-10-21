#!/bin/sh
# SPDX-License-Identifier: ISC

set -eu

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

source_file=mt7925/mlo/lifecycle.c
pair_fn=$(sed -n '/^mt7927_mlo_pair_band_idx(/,/^}/p' "$source_file")
move_fn=$(sed -n '/^mt7927_mlo_reassign_band(/,/^}/p' "$source_file")
peer_fn=$(sed -n '/^mt7927_mlo_refresh_reassigned_sta(/,/^}/p' "$source_file")
vif_fn=$(sed -n '/^int mt7927_mlo_change_vif_links(/,/^int mt7927_mlo_change_sta_links/p' "$source_file")
sta_fn=$(sed -n '/^int mt7927_mlo_change_sta_links(/,$p' "$source_file")

printf '%s\n' "$pair_fn" | grep -q 'new_links' ||
	fail "band placement is not computed from the requested active pair"
printf '%s\n' "$pair_fn" | grep -q 'NL80211_BAND_2GHZ' ||
	fail "pair placement does not reserve context 0 for 2.4 GHz"
printf '%s\n' "$pair_fn" | grep -q 'return link_id != __ffs(new_links)' ||
	fail "non-2.4 GHz pairs do not receive deterministic unique contexts"

printf '%s\n' "$move_fn" | grep -q 'mt76_connac_free_pending_tx_skbs' ||
	fail "band migration does not drain the affected peer WCID"
if printf '%s\n' "$move_fn" | tr '\n' ' ' | grep -q 'mt76_connac_mcu_uni_add_dev.*false'; then
	fail "band migration destructively disables the shared MLD device"
fi
printf '%s\n' "$move_fn" | grep -q 'mconf->mt76.band_idx = band_idx' ||
	fail "band migration does not update the host BSS context"
printf '%s\n' "$peer_fn" | grep -q 'peer->wcid.phy_idx = mconf->mt76.band_idx' ||
	fail "band migration does not update the peer WCID PHY context"
printf '%s\n' "$move_fn" | grep -q 'bmc->wcid.phy_idx = band_idx' ||
	fail "band migration does not update the BMC WCID PHY context"
printf '%s\n' "$move_fn" | tr '\n' ' ' | grep -q 'mt76_connac_mcu_uni_add_dev.*true' ||
	fail "band migration does not update the resident DEV/BSS context in place"

move_line=$(printf '%s\n' "$vif_fn" | grep -n 'mt7927_mlo_reassign_active_bands' | head -1 | cut -d: -f1 || true)
refresh_line=$(printf '%s\n' "$vif_fn" | grep -n 'mt7927_mlo_refresh_reassigned_sta' | head -1 | cut -d: -f1 || true)
roc_line=$(printf '%s\n' "$vif_fn" | grep -n 'mt7925_set_mlo_roc' | head -1 | cut -d: -f1 || true)
[ -n "$move_line" ] && [ -n "$roc_line" ] && [ "$move_line" -lt "$roc_line" ] ||
	fail "active-pair band migration does not complete before MLO ROC"
[ -n "$refresh_line" ] && [ "$move_line" -lt "$refresh_line" ] && [ "$refresh_line" -lt "$roc_line" ] ||
	fail "peer STA_REC is not rebound after BSS migration and before MLO ROC"

printf '%s\n' "$sta_fn" | grep -q 'mt7927_mlo_refresh_reassigned_sta' ||
	fail "STA_REC is not refreshed after a peer band migration"
# Publication moved into the shared recreate/authorize lifecycle. Preserve
# its PTK-before-publication contract rather than requiring a duplicate write.
printf '%s\n' "$peer_fn" | grep -q 'mt7927_mlo_recreate_authorize_peer' ||
	fail "peer refresh bypasses the shared authorization lifecycle"
bash tests/mt7925/mlo_phy_rebind_ptk_contract.sh

echo "PASS: MT7927 active pairs receive unique transactional DBDC contexts"
