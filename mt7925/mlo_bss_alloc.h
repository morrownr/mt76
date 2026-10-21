/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_MLO_BSS_ALLOC_H
#define __MT7925_MLO_BSS_ALLOC_H

/* Firmware attaches a STA_REC_MLD member only when its BSS index is below the two-link MLD table size (measured
 * 2026-10-13 with UWTBL readback: incoming link on BSS index 2 stayed "standalone" -- peer MLD = AP link address, no
 * related WCID -- in five runs; the same link on BSS index 0 attached, peer MLD = AP MLD, related WCIDs mutual).
 */
#define MT7925_MLO_MLD_BSS_SLOTS 2u

static inline unsigned int mt7925_mlo_lowest_free_bss(unsigned long long vif_mask)
{
	unsigned int idx;

	for (idx = 0; idx < 8 * sizeof(vif_mask); idx++)
		if (!(vif_mask & (1ULL << idx)))
			return idx;
	return idx;
}

static inline int mt7925_mlo_bss_idx_attachable(unsigned int bss_idx)
{
	return bss_idx < MT7925_MLO_MLD_BSS_SLOTS;
}

/* vif_mask after retiring a link whose BSS index is retired_idx */
static inline unsigned long long mt7925_mlo_vif_mask_after_retire(unsigned long long vif_mask, unsigned int retired_idx)
{
	return vif_mask & ~(1ULL << retired_idx);
}

#endif
