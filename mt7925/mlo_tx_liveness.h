/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_MLO_TX_LIVENESS_H
#define __MT7925_MLO_TX_LIVENESS_H

/* Pure decision logic for the deterministic best-link TX selector
 * (mt7925_mlo_force_bulk_wcid(), mt792x_core.c).
 *
 * Root cause this exists to fix: RSSI-only deterministic selection can pin
 * every TID to a link that has excellent signal but has stopped delivering
 * any real RX -- proven live (2026-09-12): link1 showed tx_pkts=45,
 * rx_pkts=0 for the whole test while carrying a confident RSSI score of
 * -60, and the deterministic policy pinned TID 0 to it for the entire run,
 * producing 100% ARP/ping failure. Round-robin only "worked" by accident,
 * because it also spent time on the healthy link.
 *
 * This header is deliberately free of kernel headers so the comparator can
 * be exercised by a plain userspace test binary as well as the driver
 * (see test_mlo_tx_liveness.c) -- the classification itself (reading real
 * wcid counters and jiffies) still lives in the driver; only the ordering
 * decision is factored out here.
 */

enum mt7925_mlo_tx_liveness {
	/* RX has advanced within the recent window: trustworthy right now. */
	MT7925_TX_LIVENESS_LIVE,
	/* RX advanced before, but not within the recent window: was fine,
	 * no fresh proof it still is. Better than a link with no history at
	 * all, worse than one proving itself right now.
	 */
	MT7925_TX_LIVENESS_CACHED,
	/* Never observed any RX on this link yet (newly active, or genuinely
	 * idle so far) -- no evidence either way.
	 */
	MT7925_TX_LIVENESS_UNKNOWN,
	/* TX is progressing but RX has not moved at all within the recent
	 * window despite that TX volume: proven bad, not merely untested.
	 * This is link1's actual failure mode above.
	 */
	MT7925_TX_LIVENESS_DEAD,
};

struct mt7925_tx_liveness_candidate {
	enum mt7925_mlo_tx_liveness liveness;
	long rssi;
	int link_id;
};

/* Returns true if candidate `a` should be selected over candidate `b`.
 *
 * Ordering (matches the required policy):
 *   1. liveness class: LIVE > CACHED > UNKNOWN > DEAD, unconditionally --
 *      a DEAD link's RSSI is never even compared against a LIVE link's.
 *   2. RSSI within the same liveness class: higher (less negative) wins.
 *   3. link_id as a fixed, reproducible tie-break (lower id wins) so equal
 *      liveness+RSSI never depends on loop/iteration order.
 */
static inline int
mt7925_mlo_tx_liveness_rank(enum mt7925_mlo_tx_liveness l)
{
	switch (l) {
	case MT7925_TX_LIVENESS_LIVE:
		return 3;
	case MT7925_TX_LIVENESS_CACHED:
		return 2;
	case MT7925_TX_LIVENESS_UNKNOWN:
		return 1;
	case MT7925_TX_LIVENESS_DEAD:
	default:
		return 0;
	}
}

static inline int
mt7925_mlo_tx_candidate_better(const struct mt7925_tx_liveness_candidate *a,
			       const struct mt7925_tx_liveness_candidate *b)
{
	int ra = mt7925_mlo_tx_liveness_rank(a->liveness);
	int rb = mt7925_mlo_tx_liveness_rank(b->liveness);

	if (ra != rb)
		return ra > rb;

	if (a->rssi != b->rssi)
		return a->rssi > b->rssi;

	return a->link_id < b->link_id;
}

#endif /* __MT7925_MLO_TX_LIVENESS_H */
