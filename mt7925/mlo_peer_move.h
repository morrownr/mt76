/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_MLO_PEER_MOVE_H
#define __MT7925_MLO_PEER_MOVE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

/* Decisions about a resident MLD peer when a transition re-places link band contexts (mt7927_mlo_reassign_band() /
 * mt7927_mlo_refresh_reassigned_sta()). Pure so the userspace test exercises the production code.
 *
 * A live keyed peer must never be destroyed and recreated unless its DEV/BSS really moved: recreating it in place
 * (newly=1) takes the MLD's RX down and a band-move recreate breaks the survivor (2026-09-13). wcid.phy_idx is only
 * bookkeeping that is 0 at creation, so on its own it cannot tell a moved peer from an unmoved one on band 1.
 */
static inline bool mt7925_mlo_peer_rebuild_needed(unsigned int peer_phy_idx, unsigned int link_band,
						  bool band_moved, bool skip_unmoved)
{
	if (peer_phy_idx == link_band)
		return false;
	if (skip_unmoved && !band_moved)
		return false;
	return true;
}

/* reassign_band() parks the peer's link_valid=false for the move. Refresh restores it only for peers it rebuilds; for a
 * peer it skips (phy_idx already equal to the new band) nothing would, and STA_REC_MLD would omit the live member.
 */
static inline bool mt7925_mlo_peer_restore_valid_after_move(bool knob, bool has_peer, unsigned int peer_phy_idx,
							    unsigned int new_band)
{
	return knob && has_peer && peer_phy_idx == new_band;
}

#endif
