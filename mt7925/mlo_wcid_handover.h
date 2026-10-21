/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_MLO_WCID_HANDOVER_H
#define __MT7925_MLO_WCID_HANDOVER_H

/* Pick the resident, retired deflink that an incoming link may inherit the firmware WCID from.
 *
 * Preconditions (all must hold, otherwise -1 and the stock add path creates a new WCID):
 *  - the incoming link has no resident link STA of its own,
 *  - the donor is the association's deflink (the WCID the firmware uses as MLD key/PN holder), resident and retired
 *    (link_valid = 0),
 *  - donor and incoming link use the same firmware band slot.
 * resident / live are bitmasks over link ids; band[] is the firmware band slot of each link.
 */
static inline int mt7925_mlo_handover_donor(unsigned int deflink_id, unsigned long resident, unsigned long live,
					    const unsigned int *band, unsigned int n, unsigned int incoming,
					    unsigned int incoming_band)
{
	if (incoming >= n || deflink_id >= n || incoming == deflink_id)
		return -1;
	if (resident & (1UL << incoming))
		return -1;
	if (!(resident & (1UL << deflink_id)) || (live & (1UL << deflink_id)))
		return -1;
	if (band[deflink_id] != incoming_band)
		return -1;

	return (int)deflink_id;
}

static inline int mt7925_mlo_legacy_handover_donor(void)
{
	return -1;
}

#endif
