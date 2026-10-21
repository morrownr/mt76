/* SPDX-License-Identifier: ISC */
#ifndef __MT7925_MLO_PAIR_TRANSITION_H
#define __MT7925_MLO_PAIR_TRANSITION_H

/* Pure decision logic for the MT7927 production pair transition.
 *
 * Structural affiliation (valid_links) and physical radio ownership are
 * different things. Observed channel-manager grants map 2.4 GHz to
 * dbdcband/slot 0 and both 5 GHz and 6 GHz to slot 1, so 0x3 and 0x5 are
 * the natural two-slot pairs while 0x6 time-shares slot 1.
 *
 * A transition is planned on the DELTA between the links that are usable
 * and owned now and the desired pair. A healthy survivor keeps the ROC slot
 * it already holds (aborting it turns its RX into cm=1/dec=0, and a new
 * JOIN that names it again is queued behind it by firmware and never
 * granted), so only the incoming link is acquired, on its own token.
 */

enum mt7925_mlo_band {
	MT7925_MLO_BAND_2G = 0,
	MT7925_MLO_BAND_5G = 1,
	MT7925_MLO_BAND_6G = 2,
};

/* The one place that maps a link's band to its physical slot / dbdcband. */
static inline unsigned int mt7925_mlo_band_slot(int band)
{
	return band == MT7925_MLO_BAND_2G ? 0 : 1;
}

enum mt7925_mlo_pt_state {
	MT7925_MLO_PT_IDLE,
	MT7925_MLO_PT_PREPARE,
	MT7925_MLO_PT_QUIESCE_REMOVED,
	MT7925_MLO_PT_PRESERVE_SURVIVOR,
	MT7925_MLO_PT_ACQUIRE_REPLACEMENT,
	MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER,
	MT7925_MLO_PT_WAIT_REPLACEMENT_RX,
	MT7925_MLO_PT_MIGRATE_TRAFFIC,
	MT7925_MLO_PT_RELEASE_OBSOLETE,
	MT7925_MLO_PT_CONVERGED,
	MT7925_MLO_PT_FAILED_SAFE,
	/* survivor ownership handoff (experiment, mlo_survivor_rejoin): the
	 * survivor's held channel-manager slot is released, the SAME survivor
	 * is JOINed again under a fresh token, its security state recovered,
	 * and only after secure RX returns is the incoming link requested.
	 */
	MT7925_MLO_PT_PREPARE_SURVIVOR_HANDOFF,
	MT7925_MLO_PT_RELEASE_SURVIVOR_OWNERSHIP,
	MT7925_MLO_PT_REJOIN_SURVIVOR,
	MT7925_MLO_PT_RECOVER_SURVIVOR,
	MT7925_MLO_PT_WAIT_SURVIVOR_RX,
	/* after the fresh grant the survivor's firmware DEV/BSS/BMC context
	 * (which the ROC abort leaves unbound from its channel) is rebuilt in
	 * normal-lifecycle order before the peer stages run
	 */
	MT7925_MLO_PT_REPROGRAM_SURVIVOR_CONTEXT,
	/* make-before-break (production): the security-valid grant carrying
	 * the MLD is marked as the continuity grant before anything is
	 * released, the replacement/shadow group is prepared WITHOUT aborting
	 * it, validated by secure RX, continuity is handed off, and only then
	 * is an obsolete grant released. A preparation that cannot proceed
	 * while the continuity grant is held fails safe on it.
	 */
	MT7925_MLO_PT_PRESERVE_CONTINUITY_GRANT,
	MT7925_MLO_PT_PREPARE_SHADOW_GROUP,
	MT7925_MLO_PT_VALIDATE_SHADOW_PATH,
	MT7925_MLO_PT_HANDOFF_CONTINUITY,
	MT7925_MLO_PT_RELEASE_OLD_GRANT,
	MT7925_MLO_PT_NUM_STATES,
};

static inline int
mt7925_mlo_pt_can_advance(enum mt7925_mlo_pt_state from,
			  enum mt7925_mlo_pt_state to)
{
	switch (from) {
	case MT7925_MLO_PT_IDLE:
		return to == MT7925_MLO_PT_PREPARE;
	case MT7925_MLO_PT_PREPARE:
		return to == MT7925_MLO_PT_QUIESCE_REMOVED ||
		       to == MT7925_MLO_PT_PRESERVE_SURVIVOR ||
		       to == MT7925_MLO_PT_PRESERVE_CONTINUITY_GRANT ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_PRESERVE_CONTINUITY_GRANT:
		return to == MT7925_MLO_PT_QUIESCE_REMOVED ||
		       to == MT7925_MLO_PT_PRESERVE_SURVIVOR ||
		       to == MT7925_MLO_PT_PREPARE_SHADOW_GROUP ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_QUIESCE_REMOVED:
		return to == MT7925_MLO_PT_PRESERVE_SURVIVOR ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_PRESERVE_SURVIVOR:
		return to == MT7925_MLO_PT_ACQUIRE_REPLACEMENT ||
		       to == MT7925_MLO_PT_PREPARE_SHADOW_GROUP ||
		       to == MT7925_MLO_PT_PREPARE_SURVIVOR_HANDOFF ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	/* the shadow group can only be admitted or fail safe; there is no
	 * edge from it to any release of the continuity grant
	 */
	case MT7925_MLO_PT_PREPARE_SHADOW_GROUP:
		return to == MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_VALIDATE_SHADOW_PATH:
		return to == MT7925_MLO_PT_HANDOFF_CONTINUITY ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_HANDOFF_CONTINUITY:
		return to == MT7925_MLO_PT_MIGRATE_TRAFFIC ||
		       to == MT7925_MLO_PT_RELEASE_OLD_GRANT ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_RELEASE_OLD_GRANT:
		return to == MT7925_MLO_PT_CONVERGED ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_PREPARE_SURVIVOR_HANDOFF:
		return to == MT7925_MLO_PT_RELEASE_SURVIVOR_OWNERSHIP ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_RELEASE_SURVIVOR_OWNERSHIP:
		return to == MT7925_MLO_PT_REJOIN_SURVIVOR ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_REJOIN_SURVIVOR:
		return to == MT7925_MLO_PT_REPROGRAM_SURVIVOR_CONTEXT ||
		       to == MT7925_MLO_PT_WAIT_SURVIVOR_RX ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_REPROGRAM_SURVIVOR_CONTEXT:
		return to == MT7925_MLO_PT_RECOVER_SURVIVOR ||
		       to == MT7925_MLO_PT_WAIT_SURVIVOR_RX ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_RECOVER_SURVIVOR:
		return to == MT7925_MLO_PT_WAIT_SURVIVOR_RX ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_WAIT_SURVIVOR_RX:
		return to == MT7925_MLO_PT_ACQUIRE_REPLACEMENT ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_ACQUIRE_REPLACEMENT:
		return to == MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER:
		return to == MT7925_MLO_PT_WAIT_REPLACEMENT_RX ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_WAIT_REPLACEMENT_RX:
		return to == MT7925_MLO_PT_MIGRATE_TRAFFIC ||
		       to == MT7925_MLO_PT_VALIDATE_SHADOW_PATH ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_MIGRATE_TRAFFIC:
		return to == MT7925_MLO_PT_RELEASE_OBSOLETE ||
		       to == MT7925_MLO_PT_RELEASE_OLD_GRANT ||
		       to == MT7925_MLO_PT_CONVERGED ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_RELEASE_OBSOLETE:
		return to == MT7925_MLO_PT_CONVERGED ||
		       to == MT7925_MLO_PT_FAILED_SAFE;
	case MT7925_MLO_PT_CONVERGED:
	case MT7925_MLO_PT_FAILED_SAFE:
		return to == MT7925_MLO_PT_IDLE;
	default:
		return 0;
	}
}

static inline const char *mt7925_mlo_pt_state_name(enum mt7925_mlo_pt_state s)
{
	static const char * const names[] = {
		"IDLE", "PREPARE", "QUIESCE_REMOVED", "PRESERVE_SURVIVOR",
		"ACQUIRE_REPLACEMENT", "ADMIT_REPLACEMENT_PEER",
		"WAIT_REPLACEMENT_RX", "MIGRATE_TRAFFIC", "RELEASE_OBSOLETE",
		"CONVERGED", "FAILED_SAFE", "PREPARE_SURVIVOR_HANDOFF",
		"RELEASE_SURVIVOR_OWNERSHIP", "REJOIN_SURVIVOR",
		"RECOVER_SURVIVOR", "WAIT_SURVIVOR_RX",
		"REPROGRAM_SURVIVOR_CONTEXT", "PRESERVE_CONTINUITY_GRANT",
		"PREPARE_SHADOW_GROUP", "VALIDATE_SHADOW_PATH",
		"HANDOFF_CONTINUITY", "RELEASE_OLD_GRANT",
	};

	return (unsigned int)s < MT7925_MLO_PT_NUM_STATES ? names[s] : "?";
}

/* A completion (grant, timeout, RX-ready) only mutates the transition it
 * was issued for.
 */
static inline int mt7925_mlo_pt_event_is_current(unsigned int event_gen,
						 unsigned int current_gen)
{
	return event_gen == current_gen;
}

enum mt7925_mlo_pt_kind {
	/* no pair change */
	MT7925_MLO_PT_NONE,
	/* nothing held to preserve (association setup) or a shape the delta
	 * logic does not cover: the established two-link JOIN
	 */
	MT7925_MLO_PT_FULL_JOIN,
	/* survivor slot held, one added link on an independent slot:
	 * acquire only the added link, on a fresh token
	 */
	MT7925_MLO_PT_DELTA_ACQUIRE,
	/* survivor slot held, the added link needs the SAME physical slot
	 * (5 <-> 6 on slot 1): needs a firmware retune (MLSR high-band switch,
	 * UNI 0x59) that is not yet recovered; stop safely on the survivor
	 */
	MT7925_MLO_PT_HIGHBAND_SHARED_SLOT,
	/* survivor slot AND the added link's slot are both already held (a
	 * 3-slot association-time JOIN): the pair change is a pure activation,
	 * no acquisition, no retune and no release is needed
	 */
	MT7925_MLO_PT_ALREADY_HELD,
};

struct mt7925_mlo_pt_plan {
	unsigned int removed;
	unsigned int survivor;
	unsigned int added;
	unsigned int held_survivor;
	int added_link;
	unsigned int added_slot;
	unsigned int survivor_slot;
	enum mt7925_mlo_pt_kind kind;
};

#ifdef __KERNEL__
#define MT7925_MLO_PT_HWEIGHT(x)	hweight16(x)
#define MT7925_MLO_PT_FFS(x)		__ffs(x)
#else
#define MT7925_MLO_PT_HWEIGHT(x)	__builtin_popcount(x)
#define MT7925_MLO_PT_FFS(x)		(__builtin_ctz(x))
#endif

/* @band_of_link[i] is enum mt7925_mlo_band of link i (3 links). */
static inline void
mt7925_mlo_pt_plan(unsigned int old_links, unsigned int new_links,
		   unsigned int valid_links, unsigned int held_links,
		   const int band_of_link[3], struct mt7925_mlo_pt_plan *p)
{
	p->removed = old_links & ~new_links;
	p->survivor = old_links & new_links;
	p->added = new_links & ~old_links;
	p->held_survivor = held_links & p->survivor;
	p->added_link = -1;
	p->added_slot = 0;
	p->survivor_slot = 0;
	p->kind = MT7925_MLO_PT_FULL_JOIN;

	if (old_links == new_links) {
		p->kind = MT7925_MLO_PT_NONE;
		return;
	}

	if (MT7925_MLO_PT_HWEIGHT(old_links) != 1 ||
	    MT7925_MLO_PT_HWEIGHT(new_links) != 2 ||
	    MT7925_MLO_PT_HWEIGHT(p->added) != 1 || p->removed ||
	    ((old_links | new_links) & ~valid_links) ||
	    (new_links & ~7u) || p->held_survivor != p->survivor)
		return;

	p->added_link = MT7925_MLO_PT_FFS(p->added);
	p->added_slot = mt7925_mlo_band_slot(band_of_link[p->added_link]);
	p->survivor_slot = mt7925_mlo_band_slot(
		band_of_link[MT7925_MLO_PT_FFS(p->survivor)]);
	if (held_links & p->added) {
		p->kind = MT7925_MLO_PT_ALREADY_HELD;
		return;
	}
	p->kind = p->added_slot == p->survivor_slot ?
		  MT7925_MLO_PT_HIGHBAND_SHARED_SLOT :
		  MT7925_MLO_PT_DELTA_ACQUIRE;
}

/* Survivor ownership handoff gating: only when explicitly enabled, only for
 * the delta shape (a survivor slot is held and one link on an independent
 * slot is added). Everything else keeps the existing path.
 */
static inline int
mt7925_mlo_pt_survivor_handoff_wanted(enum mt7925_mlo_pt_kind kind, int enabled,
				      unsigned int held_survivor)
{
	return enabled && kind == MT7925_MLO_PT_DELTA_ACQUIRE && held_survivor;
}

/* Firmware flips a survivor whose slot was released to cm=1/dec=0 about
 * 25 ms after the release (WTBL security state lost). A valid frame seen
 * only inside that window proves nothing; one seen at least this long after
 * the release means the survivor kept its security state.
 */
#define MT7925_MLO_PT_CM_SETTLE_MS	60

/* @last_valid_rx_after_release_ms: time of the survivor's last security-valid
 * RX relative to the release (ms), -1 when none since the release.
 */
static inline int
mt7925_mlo_pt_survivor_needs_recovery(int last_valid_rx_after_release_ms)
{
	return last_valid_rx_after_release_ms < MT7925_MLO_PT_CM_SETTLE_MS;
}

/* Bounded survivor recovery: the firmware context (DEV/BSS/BMC keys) first,
 * then the peer recreate lifecycle, then the remove-first rebuild lifecycle,
 * then fail safe. Never an open-ended retry.
 */
enum mt7925_mlo_pt_recover_stage {
	MT7925_MLO_PT_RECOVER_NONE,
	MT7925_MLO_PT_RECOVER_CONTEXT,
	MT7925_MLO_PT_RECOVER_RECREATE,
	MT7925_MLO_PT_RECOVER_REBUILD,
	MT7925_MLO_PT_RECOVER_DONE,
	MT7925_MLO_PT_RECOVER_FAILED,
};

static inline enum mt7925_mlo_pt_recover_stage
mt7925_mlo_pt_recover_next(enum mt7925_mlo_pt_recover_stage stage, int rx_ok)
{
	switch (stage) {
	case MT7925_MLO_PT_RECOVER_NONE:
		return MT7925_MLO_PT_RECOVER_CONTEXT;
	case MT7925_MLO_PT_RECOVER_CONTEXT:
		return rx_ok ? MT7925_MLO_PT_RECOVER_DONE :
			       MT7925_MLO_PT_RECOVER_RECREATE;
	case MT7925_MLO_PT_RECOVER_RECREATE:
		return rx_ok ? MT7925_MLO_PT_RECOVER_DONE :
			       MT7925_MLO_PT_RECOVER_REBUILD;
	case MT7925_MLO_PT_RECOVER_REBUILD:
		return rx_ok ? MT7925_MLO_PT_RECOVER_DONE :
			       MT7925_MLO_PT_RECOVER_FAILED;
	default:
		return MT7925_MLO_PT_RECOVER_FAILED;
	}
}

/* Sep-18 replay (test-only, mlo_sep18_replay): after the historical full
 * two-TLV JOIN is granted, the survivor is recovered with the peer lifecycle
 * only (recreate, then remove-first rebuild). The DEV/BSS/BMC context stage
 * is deliberately absent: the JOIN itself re-granted the survivor's channel
 * binding, and the historical sequence never re-sent survivor DEV/BSS.
 */
static inline enum mt7925_mlo_pt_recover_stage
mt7925_mlo_pt_peer_recover_next(enum mt7925_mlo_pt_recover_stage stage, int rx_ok)
{
	switch (stage) {
	case MT7925_MLO_PT_RECOVER_NONE:
		return MT7925_MLO_PT_RECOVER_RECREATE;
	case MT7925_MLO_PT_RECOVER_RECREATE:
		return rx_ok ? MT7925_MLO_PT_RECOVER_DONE :
			       MT7925_MLO_PT_RECOVER_REBUILD;
	case MT7925_MLO_PT_RECOVER_REBUILD:
		return rx_ok ? MT7925_MLO_PT_RECOVER_DONE :
			       MT7925_MLO_PT_RECOVER_FAILED;
	default:
		return MT7925_MLO_PT_RECOVER_FAILED;
	}
}

/* Post-regrant security rebind (test gate mlo_security_rebind).
 *
 * Releasing the old multi-link privilege and re-granting through a fresh
 * JOIN gives every active link a new firmware radio/channel incarnation.
 * The logical MLD security state (PTK/GTK material, association) is
 * unchanged, but the firmware-local security binding of BOTH active links
 * -- the survivor included -- is stale. Rebind covers the whole active set.
 */
static inline unsigned int
mt7925_mlo_pt_rebind_links(int radio_group_reincarnated, unsigned int active_links)
{
	return radio_group_reincarnated ? active_links : 0;
}

/* A host hw_key_idx that merely looks valid must not suppress reprogramming
 * once the firmware incarnation changed; an unset index always needs it.
 */
static inline int
mt7925_mlo_pt_key_reprogram_needed(unsigned int hw_key_idx, int reincarnated)
{
	return reincarnated || hw_key_idx == 255;
}

/* STA_REC_MLD membership: mac80211 publishes the new active set only after
 * the driver callbacks return, so during the rebind the record must be built
 * from the post-grant target set when one is pending.
 */
static inline unsigned int
mt7925_mlo_pt_mld_links(unsigned int published_active, unsigned int rebind_target)
{
	return published_active | rebind_target;
}

enum mt7925_mlo_pt_rebind_phase {
	MT7925_MLO_PT_REBIND_NONE,
	MT7925_MLO_PT_REBIND_INVALIDATE,
	MT7925_MLO_PT_REBIND_STA_REC,
	MT7925_MLO_PT_REBIND_PTK,
	MT7925_MLO_PT_REBIND_GTK,
	MT7925_MLO_PT_REBIND_AUTHORIZE,
	MT7925_MLO_PT_REBIND_WAIT_RX,
	MT7925_MLO_PT_REBIND_DONE,
	MT7925_MLO_PT_REBIND_FAILED,
};

static inline enum mt7925_mlo_pt_rebind_phase
mt7925_mlo_pt_rebind_next(enum mt7925_mlo_pt_rebind_phase phase, int last_err)
{
	if (last_err)
		return MT7925_MLO_PT_REBIND_FAILED;

	switch (phase) {
	case MT7925_MLO_PT_REBIND_NONE:
		return MT7925_MLO_PT_REBIND_INVALIDATE;
	case MT7925_MLO_PT_REBIND_INVALIDATE:
		return MT7925_MLO_PT_REBIND_STA_REC;
	case MT7925_MLO_PT_REBIND_STA_REC:
		return MT7925_MLO_PT_REBIND_PTK;
	case MT7925_MLO_PT_REBIND_PTK:
		return MT7925_MLO_PT_REBIND_GTK;
	case MT7925_MLO_PT_REBIND_GTK:
		return MT7925_MLO_PT_REBIND_AUTHORIZE;
	case MT7925_MLO_PT_REBIND_AUTHORIZE:
		return MT7925_MLO_PT_REBIND_WAIT_RX;
	case MT7925_MLO_PT_REBIND_WAIT_RX:
	case MT7925_MLO_PT_REBIND_DONE:
		return MT7925_MLO_PT_REBIND_DONE;
	default:
		return MT7925_MLO_PT_REBIND_FAILED;
	}
}

/* Post-regrant parent (DEV/BSS) rebuild (test gate mlo_regrant_parent_rebuild).
 *
 * A full radio-group regrant gives every active link a new firmware channel
 * incarnation. The DEV_INFO/BSS_INFO parent context of BOTH links -- the
 * survivor included -- is rebuilt against the new grants (DEV then BSS),
 * and only once that parent pass is DONE may the child security phases
 * (STA_REC, STA_REC_MLD, keys, authorize, RX gate) run.
 */
static inline unsigned int
mt7925_mlo_pt_parent_links(int parent_rebuild, unsigned int rebind_links)
{
	return parent_rebuild ? rebind_links : 0;
}

enum mt7925_mlo_pt_parent_step {
	MT7925_MLO_PT_PARENT_NONE,
	MT7925_MLO_PT_PARENT_DEV,
	MT7925_MLO_PT_PARENT_BSS,
	MT7925_MLO_PT_PARENT_DONE,
	MT7925_MLO_PT_PARENT_FAILED,
};

static inline enum mt7925_mlo_pt_parent_step
mt7925_mlo_pt_parent_next(enum mt7925_mlo_pt_parent_step step, int last_err)
{
	if (last_err)
		return MT7925_MLO_PT_PARENT_FAILED;

	switch (step) {
	case MT7925_MLO_PT_PARENT_NONE:
		return MT7925_MLO_PT_PARENT_DEV;
	case MT7925_MLO_PT_PARENT_DEV:
		return MT7925_MLO_PT_PARENT_BSS;
	case MT7925_MLO_PT_PARENT_BSS:
	case MT7925_MLO_PT_PARENT_DONE:
		return MT7925_MLO_PT_PARENT_DONE;
	default:
		return MT7925_MLO_PT_PARENT_FAILED;
	}
}

static inline int
mt7925_mlo_pt_child_allowed(int parent_rebuild,
			    enum mt7925_mlo_pt_parent_step parent)
{
	return !parent_rebuild || parent == MT7925_MLO_PT_PARENT_DONE;
}

/* Whole post-regrant reconstruction sequence, parent before child. */
enum mt7925_mlo_pt_seq_step {
	MT7925_MLO_PT_SEQ_DEV,
	MT7925_MLO_PT_SEQ_BSS,
	MT7925_MLO_PT_SEQ_STA_REC,
	MT7925_MLO_PT_SEQ_STA_REC_MLD,
	MT7925_MLO_PT_SEQ_KEYS,
	MT7925_MLO_PT_SEQ_AUTHORIZE,
	MT7925_MLO_PT_SEQ_RX_GATE,
};

static inline int
mt7925_mlo_pt_regrant_seq_ok(const int *seq, unsigned int n)
{
	static const int want[] = {
		MT7925_MLO_PT_SEQ_DEV, MT7925_MLO_PT_SEQ_BSS,
		MT7925_MLO_PT_SEQ_STA_REC, MT7925_MLO_PT_SEQ_STA_REC_MLD,
		MT7925_MLO_PT_SEQ_KEYS, MT7925_MLO_PT_SEQ_AUTHORIZE,
		MT7925_MLO_PT_SEQ_RX_GATE,
	};
	unsigned int i;

	if (n != sizeof(want) / sizeof(want[0]))
		return 0;
	for (i = 0; i < n; i++)
		if (seq[i] != want[i])
			return 0;
	return 1;
}

/* The transition converges only when every active link has proven
 * security-valid RX; one healthy link never vouches for the other.
 */
static inline int
mt7925_mlo_pt_converge_allowed(unsigned int active_links, unsigned int secure_rx_links)
{
	return active_links && !(active_links & ~secure_rx_links);
}

static inline int
mt7925_mlo_pt_sep18_recovery_wanted(int replay_enabled, int survivor_rx_after_grant)
{
	return replay_enabled && !survivor_rx_after_grant;
}

/* Order of the survivor firmware-context reprogram, taken from the normal
 * link lifecycle (mt7925_mac_link_bss_add DEV_INFO, then BSS_INFO with the
 * channel/RLM binding and MLD tlv, then the BMC group keys) and the proven
 * band-move path (mt7927_mlo_reassign_band). A failing step stops the
 * sequence: no further MCU commands are sent after an error.
 */
enum mt7925_mlo_pt_ctx_step {
	MT7925_MLO_PT_CTX_NONE,
	MT7925_MLO_PT_CTX_DEV,
	MT7925_MLO_PT_CTX_BSS,
	MT7925_MLO_PT_CTX_BMC_KEYS,
	MT7925_MLO_PT_CTX_DONE,
	MT7925_MLO_PT_CTX_FAILED,
};

static inline enum mt7925_mlo_pt_ctx_step
mt7925_mlo_pt_ctx_next(enum mt7925_mlo_pt_ctx_step step, int last_err)
{
	if (last_err)
		return MT7925_MLO_PT_CTX_FAILED;

	switch (step) {
	case MT7925_MLO_PT_CTX_NONE:
		return MT7925_MLO_PT_CTX_DEV;
	case MT7925_MLO_PT_CTX_DEV:
		return MT7925_MLO_PT_CTX_BSS;
	case MT7925_MLO_PT_CTX_BSS:
		return MT7925_MLO_PT_CTX_BMC_KEYS;
	case MT7925_MLO_PT_CTX_BMC_KEYS:
	case MT7925_MLO_PT_CTX_DONE:
		return MT7925_MLO_PT_CTX_DONE;
	default:
		return MT7925_MLO_PT_CTX_FAILED;
	}
}

enum mt7925_mlo_pt_grant {
	MT7925_MLO_PT_GRANT_OK,
	MT7925_MLO_PT_GRANT_STALE_TOKEN,
	MT7925_MLO_PT_GRANT_WRONG_BSS,
	MT7925_MLO_PT_GRANT_FW_STATUS,
	MT7925_MLO_PT_GRANT_WRONG_SLOT,
};

/* Validate a firmware event against the one outstanding delta request. */
static inline int
mt7925_mlo_pt_grant_check(unsigned int exp_token, unsigned int exp_bss,
			  unsigned int exp_slot, unsigned int ev_token,
			  unsigned int ev_bss, unsigned int ev_dbdcband,
			  unsigned int ev_status)
{
	if (!exp_token || ev_token != exp_token)
		return MT7925_MLO_PT_GRANT_STALE_TOKEN;
	if (ev_bss != exp_bss)
		return MT7925_MLO_PT_GRANT_WRONG_BSS;
	if (ev_status)
		return MT7925_MLO_PT_GRANT_FW_STATUS;
	if (ev_dbdcband != exp_slot)
		return MT7925_MLO_PT_GRANT_WRONG_SLOT;
	return MT7925_MLO_PT_GRANT_OK;
}

/* ---- make-before-break ---- */

/* A survivor counts as security-valid (a continuity grant) when its last
 * security-valid RX is no older than this.
 */
#define MT7925_MLO_PT_SECURE_RX_FRESH_MS	2000

static inline int
mt7925_mlo_pt_secure_rx_fresh(int has_valid_rx, unsigned int age_ms)
{
	return has_valid_rx && age_ms <= MT7925_MLO_PT_SECURE_RX_FRESH_MS;
}

/* Active mask that is (or, once mac80211 unwinds a failed grow, will be)
 * really in effect. Inside change_vif_links mac80211 has already OR'ed the
 * added links into vif->active_links; an added link without a physical
 * grant is never part of the published state.
 */
static inline unsigned int
mt7925_mlo_pt_published_active(unsigned int vif_active, unsigned int added,
			       unsigned int granted)
{
	return vif_active & ~(added & ~granted);
}

enum mt7925_mlo_pt_reason {
	MT7925_MLO_PT_REASON_NONE,
	/* the replacement/shadow context could not be prepared while the
	 * continuity grant is held (firmware silence / rejection)
	 */
	MT7925_MLO_PT_REASON_PREPARE_BLOCKED_BY_HELD_GRANT,
	MT7925_MLO_PT_REASON_PREPARE_FAILED,
};

/* Outcome of PREPARE_SHADOW_GROUP: success admits the replacement; any
 * failure fails safe on the untouched continuity grant. Never a fallback.
 */
static inline enum mt7925_mlo_pt_state
mt7925_mlo_pt_prepare_result(int err, int continuity_held)
{
	(void)continuity_held;
	return err ? MT7925_MLO_PT_FAILED_SAFE :
		     MT7925_MLO_PT_ADMIT_REPLACEMENT_PEER;
}

static inline enum mt7925_mlo_pt_reason
mt7925_mlo_pt_prepare_reason(int err, int continuity_held)
{
	if (!err)
		return MT7925_MLO_PT_REASON_NONE;
	return continuity_held ?
	       MT7925_MLO_PT_REASON_PREPARE_BLOCKED_BY_HELD_GRANT :
	       MT7925_MLO_PT_REASON_PREPARE_FAILED;
}

/* Destructive behaviour (releasing the continuity grant: release-all at the
 * shrink, survivor re-JOIN, Sep-18 replay and its rebind/rebuild add-ons)
 * exists only behind explicit test gates. mlo_roc_release bit 8 is the
 * legacy release-everything A/B knob; production default (3) never sets it.
 */
static inline int
mt7925_mlo_pt_destructive_gate(int sep18_replay, int security_rebind,
			       int parent_rebuild, int survivor_rejoin,
			       unsigned int roc_release)
{
	return sep18_replay || security_rebind || parent_rebuild ||
	       survivor_rejoin || !!(roc_release & 8);
}

/* A failed make-before-break preparation never falls back to the
 * destructive release-all + full JOIN path unless a test gate asks for it.
 */
static inline int mt7925_mlo_pt_destructive_fallback_allowed(int gate)
{
	return !!gate;
}

#endif
