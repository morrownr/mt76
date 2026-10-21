/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_MLO_RETOUCH_PRIMARY_H
#define __MT7925_MLO_RETOUCH_PRIMARY_H

#include "mlo_tx_primary.h"

/* Which link's STA_REC carries the primary "retouch" (the update that lists the incoming partner in STA_REC_MLD).
 *
 * The retouch must go to a live MLD member: the MLD record's own primary (see mt7925_mcu_sta_mld_tlv()) is chosen from
 * the live members, and firmware only rewrites a WCID's MLD relations when that WCID's STA_REC is updated. Addressing
 * the retired host deflink leaves the survivor's relations pointing at the retired WCID.
 *
 * Returns the link id to retouch, or -1 when no live member exists (initial association has not published active
 * links yet); the caller then keeps its legacy subject.
 */
static inline int mt7925_mlo_retouch_primary_link(unsigned int deflink_id, unsigned long active,
						  unsigned long valid, unsigned int incoming)
{
	if (incoming < 8 * sizeof(unsigned long))
		active &= ~(1UL << incoming);

	return mt7925_mlo_tx_primary_link(deflink_id, active, valid);
}

/* Stock behaviour, kept only so the unit test can show what changed. */
static inline int mt7925_mlo_legacy_retouch_primary_link(unsigned int deflink_id)
{
	return (int)deflink_id;
}

#endif
