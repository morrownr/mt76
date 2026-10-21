/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_MLO_RX_FREQ_RESOLVE_H
#define __MT7925_MLO_RX_FREQ_RESOLVE_H

/* Attribute a received unicast frame to the affiliated link by the channel it was received on.
 *
 * After a pair transition the firmware can keep reporting the retired link's WLAN_IDX for frames of the new link (both
 * share a firmware band slot), e.g. wcid1 / link 2 for every frame received on the new 5 GHz link. mac80211 derives the
 * RX link from the WCID, so the frames land on an inactive link. The physical channel of the reception is the one
 * attribute that does not depend on the firmware's lookup.
 *
 * One candidate per link id. usable = the link exists, its WCID is link_valid and the link is in the active set.
 */
struct mt7925_mlo_rx_freq_cand {
	int usable;
	unsigned int freq;
};

/* Returns the link id to attribute the frame to, or -1 to keep the firmware's choice (unknown or ambiguous frequency,
 * no frequency available).
 */
static inline int mt7925_mlo_rx_freq_select(const struct mt7925_mlo_rx_freq_cand *cands, unsigned int n,
					    unsigned int rx_freq, unsigned int resolved_link)
{
	int found = -1;
	unsigned int i;

	if (!rx_freq)
		return -1;

	for (i = 0; i < n; i++) {
		if (!cands[i].usable || cands[i].freq != rx_freq)
			continue;
		if (found >= 0)
			return -1;
		found = (int)i;
	}

	/* found == resolved_link means the firmware's attribution is already right; the caller treats it as "no change" */
	(void)resolved_link;
	return found;
}

/* Stock behaviour for the test: the firmware's WLAN_IDX link is used unchanged. */
static inline int mt7925_mlo_legacy_rx_freq_select(unsigned int resolved_link)
{
	return (int)resolved_link;
}

#endif
