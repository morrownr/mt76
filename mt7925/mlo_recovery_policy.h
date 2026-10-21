/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7925_MLO_RECOVERY_POLICY_H
#define __MT7925_MLO_RECOVERY_POLICY_H

/* Pure decision logic for the MT7927 post-failed-admission recovery watch
 * (mlo/recovery.c). No kernel headers, so it is exercised by a userspace test.
 *
 * Observed (hw, 2026-10-18): after a replacement link fails admission and the
 * survivor mask is restored, the survivor keeps transmitting but its
 * security-valid RX never advances, a full scan returns 0 BSS, and only a
 * firmware reset clears it. A reset reassociates, so it is the last resort:
 * evidence-gated (TX volume with zero RX over a full window), once per
 * association, and rate limited across associations.
 */

#define MT7925_MLO_REC_WINDOW_MS	5000u	/* RX must move within this */
#define MT7925_MLO_REC_HEALTHY_MS	10000u	/* RX must keep moving this long to end the watch */
#define MT7925_MLO_REC_MIN_TX		10u	/* TX evidence needed to call it dead */
#define MT7925_MLO_REC_MAX_WATCH_MS	60000u	/* give up on an idle survivor */
#define MT7925_MLO_REC_MIN_INTERVAL_MS	300000u	/* between resets, any association */
#define MT7925_MLO_REC_LOST_MIN_MS	2000u	/* evidence age that makes a lost association count */
#define MT7925_MLO_REASSOC_MIN_INTERVAL_MS 120000u	/* between reassociation fallbacks */

enum mt7925_mlo_rec_action {
	MT7925_MLO_REC_IDLE,	/* nothing armed */
	MT7925_MLO_REC_WAIT,	/* keep watching */
	MT7925_MLO_REC_HEALTHY,	/* survivor RX advanced: stop */
	MT7925_MLO_REC_RESET,	/* dead: request the firmware reset */
	MT7925_MLO_REC_BACKOFF,	/* dead, but a reset is not allowed: stop */
	MT7925_MLO_REC_GIVE_UP,	/* no TX evidence within the watch limit: stop */
	MT7925_MLO_REC_CANCEL,	/* association changed: stop */
};

struct mt7925_mlo_recovery {
	int armed;
	unsigned int rx_base, tx_base;
	unsigned long long start_ms;
	unsigned long long epoch;
	unsigned int rx_last;			/* RX counter at the last movement */
	unsigned int tx_mark;			/* TX counter at the last RX movement */
	unsigned long long last_move_ms;	/* when RX last moved */
	unsigned int tx_last;			/* last sample taken while armed */
	unsigned long long t_last;
	unsigned int resets;	/* in this association */
};

static inline void
mt7925_mlo_recovery_arm(struct mt7925_mlo_recovery *r, unsigned int rx, unsigned int tx,
			unsigned long long now_ms, unsigned long long epoch)
{
	/* The once-per-association limit belongs to an association. */
	if (epoch != r->epoch)
		r->resets = 0;
	r->armed = 1;
	r->rx_base = rx;
	r->tx_base = tx;
	r->tx_last = tx;
	r->rx_last = rx;
	r->tx_mark = tx;
	r->start_ms = now_ms;
	r->last_move_ms = now_ms;
	r->t_last = now_ms;
	r->epoch = epoch;
}

/* Dead: one reset per association and a minimum spacing between resets. */
static inline enum mt7925_mlo_rec_action
mt7925_mlo_recovery_decide_reset(struct mt7925_mlo_recovery *r, unsigned long long ms_since_reset)
{
	if (r->resets || ms_since_reset < MT7925_MLO_REC_MIN_INTERVAL_MS)
		return MT7925_MLO_REC_BACKOFF;
	r->resets++;
	return MT7925_MLO_REC_RESET;
}

/* One evaluation. ms_since_reset is how long ago the last reset (any
 * association) happened; pass a large value if none.
 */
static inline enum mt7925_mlo_rec_action
mt7925_mlo_recovery_step(struct mt7925_mlo_recovery *r, unsigned int rx, unsigned int tx,
			 unsigned long long now_ms, unsigned long long epoch,
			 unsigned long long ms_since_reset)
{
	unsigned long long idle;

	if (!r->armed)
		return MT7925_MLO_REC_IDLE;
	if (epoch != r->epoch) {
		/* The association went away while armed. Hardware showed the radio
		 * stays dead after the disassociation, so TX-without-RX already seen
		 * for long enough still counts; anything less is just a cancel.
		 */
		r->armed = 0;
		if ((unsigned int)(r->tx_last - r->tx_mark) >= MT7925_MLO_REC_MIN_TX &&
		    r->t_last - r->last_move_ms >= MT7925_MLO_REC_LOST_MIN_MS &&
		    rx == r->rx_last)
			return mt7925_mlo_recovery_decide_reset(r, ms_since_reset);
		return MT7925_MLO_REC_CANCEL;
	}
	/* RX movement restarts the window; it ends the watch only when it keeps
	 * going for HEALTHY_MS (hardware: RX still arrived for about a second after
	 * a failed add and then stopped for good).
	 */
	if (rx != r->rx_last) {
		r->rx_last = rx;
		r->last_move_ms = now_ms;
		r->tx_mark = tx;
		r->tx_last = tx;
		r->t_last = now_ms;
		if (now_ms - r->start_ms >= MT7925_MLO_REC_HEALTHY_MS) {
			r->armed = 0;
			return MT7925_MLO_REC_HEALTHY;
		}
		return MT7925_MLO_REC_WAIT;
	}
	r->tx_last = tx;
	r->t_last = now_ms;
	idle = now_ms - r->last_move_ms;
	if (idle < MT7925_MLO_REC_WINDOW_MS)
		return MT7925_MLO_REC_WAIT;
	if ((unsigned int)(tx - r->tx_mark) < MT7925_MLO_REC_MIN_TX) {
		if (now_ms - r->start_ms >= MT7925_MLO_REC_MAX_WATCH_MS) {
			r->armed = 0;
			return MT7925_MLO_REC_GIVE_UP;
		}
		return MT7925_MLO_REC_WAIT;
	}
	r->armed = 0;
	return mt7925_mlo_recovery_decide_reset(r, ms_since_reset);
}

/* A failed grant with a healthy radio is answered with one reassociation, never a
 * firmware reset. since_last_ms is the time since the previous fallback, or ~0
 * when there was none. The interval is shorter than the reset interval because a
 * reassociation does not reload the firmware, but it still has to be rare: it is
 * what a user sees on the AP and the router.
 */
static inline int
mt7925_mlo_reassoc_allowed(unsigned long long since_last_ms)
{
	return since_last_ms >= MT7925_MLO_REASSOC_MIN_INTERVAL_MS;
}

#endif
