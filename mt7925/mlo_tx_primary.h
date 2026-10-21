/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_MLO_TX_PRIMARY_H
#define __MT7925_MLO_TX_PRIMARY_H

/* Which of an MLD station's link WCIDs a unicast data frame is addressed with.
 *
 * MT7927 firmware keeps one TX PN (and SN) counter per UWTBL entry, i.e. per link WCID, but an MLD's links share one
 * PTKSA, so the AP replays-checks against the highest PN it has seen from ANY link. Addressing a frame with a secondary
 * link's WCID therefore draws its PN from a counter that is unrelated to the primary's and the AP silently drops it
 * until that counter overtakes the other one (measured 2026-10-13: forced secondary pings lost exactly the first 27
 * frames, the PN gap). Frames addressed with the primary (MLD) WCID are scheduled over the radios by the firmware with a
 * single PN space and were measured carrying data on both links of a 2.4+5 GHz association.
 *
 * The result must follow the same primary that STA_REC_MLD announces to firmware: the station's default link while it is
 * active and its WCID is valid, otherwise the lowest-numbered active link with a valid WCID. An inactive link is never
 * returned. Returns the link id or -1 when no link is usable.
 *
 * Pure and free of kernel headers so the userspace test (test_mlo_tx_primary.c) exercises the production function.
 */
static inline int mt7925_mlo_tx_primary_link(unsigned int deflink_id,
					     unsigned long active,
					     unsigned long valid)
{
	unsigned long usable = active & valid;
	unsigned int id;

	if (!usable)
		return -1;

	if (deflink_id < 8 * sizeof(unsigned long) && (usable & (1UL << deflink_id)))
		return (int)deflink_id;

	for (id = 0; id < 8 * sizeof(unsigned long); id++)
		if (usable & (1UL << id))
			return (int)id;

	return -1;
}

#endif
