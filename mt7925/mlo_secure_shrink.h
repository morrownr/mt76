/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7925_MLO_SECURE_SHRINK_H
#define __MT7925_MLO_SECURE_SHRINK_H

/* Pure decision logic: may a host-requested active-link transition proceed
 * without destroying the only authenticated datapath?
 *
 * mac80211 always shrinks to (old & new) before growing to @new, and a
 * driver cannot veto that shrink once it has started (drv_change_vif_links
 * errors during a shrink are WARN_ON_ONCE'd and swallowed). The only
 * place a transition can still be refused cleanly is before it is
 * requested.
 *
 * @secure is the set of links, among @old, whose current WCID incarnation
 * delivered fresh security-valid unicast RX (mlo_valid_rx_*: excludes
 * cipher-mismatch / ICV-failed frames, null frames and counters that merely
 * resolve to a WCID). A link that never proved secure RX does not count.
 *
 * Observed 2026-10-04: 0x6 -> 0x3 with L2 secure (the only one) and
 * survivor L1 not secure. The continuity guard only protected the survivor
 * slot, so the secure departing L2 was released.
 */

enum mt7925_mlo_secure_verdict {
	MT7925_MLO_SECURE_ALLOW_NO_SECURE_PATH,		/* nothing authenticated to preserve */
	MT7925_MLO_SECURE_ALLOW_SURVIVOR_SECURE,	/* a secure link survives the shrink */
	MT7925_MLO_SECURE_VETO_SECURE_DEPARTS,		/* only secure link(s) would be released */
};

static inline int
mt7925_mlo_secure_transition_verdict(unsigned int old_active,
				     unsigned int new_active,
				     unsigned int secure)
{
	unsigned int old_secure = old_active & secure;
	unsigned int survivor = old_active & new_active;

	if (old_active == new_active || !old_secure)
		return MT7925_MLO_SECURE_ALLOW_NO_SECURE_PATH;
	if (survivor & old_secure)
		return MT7925_MLO_SECURE_ALLOW_SURVIVOR_SECURE;
	return MT7925_MLO_SECURE_VETO_SECURE_DEPARTS;
}

static inline int mt7925_mlo_secure_verdict_allows(int v)
{
	return v != MT7925_MLO_SECURE_VETO_SECURE_DEPARTS;
}

static inline const char *mt7925_mlo_secure_verdict_name(int v)
{
	static const char * const n[] = {
		"no_secure_path", "survivor_secure", "secure_link_would_depart",
	};

	return (unsigned int)v <= MT7925_MLO_SECURE_VETO_SECURE_DEPARTS ? n[v] : "?";
}

/* A rollback to @target is only settled when the published mask is @target
 * AND a link of @target proved security-valid RX AFTER the rollback was
 * requested (@secure_since_request). The host mask alone is not proof:
 * mac80211 transiently ORs a being-added link into active_links and
 * reverts it when the driver later fails the grow
 * (MLSR_HIGH_BAND_SWITCH_REQUIRED / -95 in the 2026-10-04 run).
 */
static inline int
mt7925_mlo_rollback_restored(unsigned int active, unsigned int target,
			     unsigned int secure_since_request)
{
	return active == target && (secure_since_request & target) != 0;
}

#endif
