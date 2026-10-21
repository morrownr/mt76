/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_MLO_LINK_READINESS_H
#define __MT7925_MLO_LINK_READINESS_H

/* Pure decision logic mirroring mt7925_mlo_link_tx_stable() (mt792x_core.c),
 * isolated for contract testing the ORDERING fix in
 * mt7927_mlo_refresh_reassigned_sta() (mt7927_mlo.c).
 *
 * Root cause this exists to fix (proven live, 2026-09-12, automatic 0x6->0x3
 * switch at 15:09:12): a survivor link undergoing PHY migration (force_newly
 * peer recreation, group-key rebind, PTK reconciliation) had
 * peer->wcid.link_valid set to true BEFORE the bounded RX-progress
 * convergence loop (MLO_PHY_REBIND_CONVERGE) even started. The TX selector's
 * only other gate, mt7925_mlo_link_tx_stable(), reads a per-link readiness
 * timestamp that is stamped ONCE at first-ever WCID creation and was never
 * rearmed by the migration path -- so it read as "long stable" from stale
 * history the instant link_valid flipped true, mid-reconciliation. Bulk TCP
 * dispatch collapsed from ~8000/s to ~1/s within the same second and never
 * recovered, ~10 seconds before MLO_STEER_COMMIT even ran.
 *
 * The fix reuses this exact existing mechanism (do not invent a new one):
 * clear the readiness timestamp to 0 the instant migration goes destructive,
 * and only restamp it to "now" after the convergence loop finishes -- so the
 * existing MT7925_MLO_TX_STABILIZE_MS grace window (already proven correct
 * for brand-new link admission) also covers a migrating survivor link,
 * instead of being silently bypassed by its stale old timestamp.
 */

static inline int
mt7925_mlo_link_tx_ready(unsigned long ready_ts, unsigned long now,
			 unsigned long stabilize_ticks)
{
	if (!ready_ts)
		return 0;

	return now > ready_ts + stabilize_ticks;
}

/* Models the ordering contract itself: given the three lifecycle timestamps
 * (migration start, convergence end, "now"), what does the readiness
 * timestamp equal, and is the link TX-ready at each point? This is the part
 * a naive "just check link_valid" implementation gets wrong -- readiness
 * must never observably precede convergence completion.
 */
struct mt7925_link_migration_timeline {
	unsigned long migration_start_ts;
	unsigned long link_valid_set_ts;   /* when wcid.link_valid = true runs */
	unsigned long convergence_end_ts;  /* when the backoff loop finishes */
};

/* Correct (fixed) behaviour: ready_ts is 0 from migration_start_ts through
 * convergence_end_ts, and becomes convergence_end_ts only after that point.
 * link_valid_set_ts (which happens strictly before convergence_end_ts in the
 * real function) must have NO effect on this value at all.
 */
static inline unsigned long
mt7925_mlo_link_ready_ts_after_fix(const struct mt7925_link_migration_timeline *t,
				   unsigned long now)
{
	if (now < t->convergence_end_ts)
		return 0;

	return t->convergence_end_ts;
}

#endif /* __MT7925_MLO_LINK_READINESS_H */
