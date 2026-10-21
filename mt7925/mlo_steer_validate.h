/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_MLO_STEER_VALIDATE_H
#define __MT7925_MLO_STEER_VALIDATE_H

/* Pure decision logic for per-link steering-transition validation
 * (mt7925_mlo_steer_switch_validate(), main.c).
 *
 * Root cause this exists to fix: validation required EVERY member of the
 * newly requested pair to independently show tx_pkts_delta >= MIN_PKTS AND
 * rx_pkts_delta >= MIN_PKTS during the VERIFY window. But the deterministic
 * best-link TX selector (mlo_tx_liveness.h) correctly leaves an already-live
 * survivor link's TID pin untouched across the transition -- proven live
 * (2026-09-12): after an automatic 0x6->0x3 switch, link1 (survivor, already
 * LIVE before the switch) kept the sole TID 0 pin and carried all real
 * traffic (tx=59 rx=24, pass); link0 (freshly admitted) received exactly
 * zero TX dispatch attempts the entire window (tx=0 rx=0) -- not because its
 * RX was dead, but because it was simply never selected to carry anything.
 * The old validation collapsed "never exercised" into the same bucket as
 * "exercised and failed," so this correctly-idle link caused the whole pair
 * to fail validation, triggering an unnecessary rollback whose own reverse
 * migration then failed to converge and cascaded into a real
 * locally-generated disconnect/reassociation.
 *
 * This header is deliberately free of kernel headers so the classifier can
 * be exercised by a plain userspace test binary as well as the driver (see
 * test_mlo_steer_validate.c).
 */

enum mt7925_mlo_link_validate_result {
	/* Real, recent, bidirectional progress: this link is proven. */
	MT7925_LINK_VALIDATE_PASS,
	/* Had a real chance (tx progressed) but rx did not, or progress was
	 * not recent enough: this link is proven BAD.
	 */
	MT7925_LINK_VALIDATE_FAIL,
	/* tx_delta == 0 AND rx_delta == 0: never exercised at all. This is
	 * NOT evidence of failure -- a deterministic scheduler leaving one
	 * pair member idle is expected, correct behaviour, not a broken
	 * link. Must never be collapsed into FAIL.
	 */
	MT7925_LINK_VALIDATE_DEFERRED,
};

static inline enum mt7925_mlo_link_validate_result
mt7925_mlo_classify_link_validation(long tx_delta, long rx_delta,
				    long min_pkts, long dead_tx_threshold,
				    long recent_ms, long recent_window_ms)
{
	if (tx_delta == 0 && rx_delta == 0)
		return MT7925_LINK_VALIDATE_DEFERRED;

	/* Real, recent RX is the harder-to-fake proof: the driver can
	 * address a wcid directly for TX regardless of firmware peer state
	 * (proven all session -- every "TX fine, RX silent" bug this project
	 * found), but firmware only attributes RX to a wcid with a genuinely
	 * working peer/WTBL entry. A link receiving real recent RX is proven
	 * live regardless of whether the TX selector also chose to send
	 * through it this window -- proven live (2026-09-12): link0 showed
	 * tx_pkts_delta=0 rx_pkts_delta=133 (real AP-initiated downlink
	 * delivered correctly) and the OLD both-directions-required rule
	 * wrongly failed it.
	 */
	if (rx_delta >= min_pkts && recent_ms <= recent_window_ms)
		return MT7925_LINK_VALIDATE_PASS;

	/* TX progressing well beyond a real chance, with zero RX at all:
	 * the proven-bad pattern from the original WLAN_IDX=4095
	 * investigation (link kept transmitting into a black hole).
	 */
	if (tx_delta >= dead_tx_threshold && rx_delta == 0)
		return MT7925_LINK_VALIDATE_FAIL;

	/* Some activity, but not enough evidence either way yet -- do not
	 * guess; a link this thinly exercised has not proven anything.
	 */
	return MT7925_LINK_VALIDATE_DEFERRED;
}

/* Pair-level commit decision: PAIR FORWARDING vs PER-LINK READINESS are
 * different facts. A transition commits when no member is proven FAIL and
 * at least one member is proven PASS (the pair is actually usable right
 * now) -- a DEFERRED member neither blocks commit nor proves anything by
 * itself; it stays untrusted for its OWN later selection, but does not
 * doom a pair that is otherwise demonstrably working.
 */
static inline int
mt7925_mlo_pair_validate_ok(int any_fail, int any_pass)
{
	return !any_fail && any_pass;
}

#endif /* __MT7925_MLO_STEER_VALIDATE_H */
