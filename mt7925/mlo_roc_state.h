/* SPDX-License-Identifier: ISC */
#ifndef __MT7925_MLO_ROC_STATE_H
#define __MT7925_MLO_ROC_STATE_H

/* Firmware-granted (held) and in-flight (pending) MLO JOIN ROC state, kept
 * apart on purpose. Firmware keys a reservation by (token, bss_idx): a
 * request must never be recorded as held before it is granted, and aborts
 * must be sent under the token that actually owns the slot. After a
 * selective release the survivor's slot is still owned by the OLD token.
 *
 * held_token/held_links   slots firmware has granted and not yet released
 * pending_token/_links    a request that has been sent but not granted
 * timedout_token          token of the last request that timed out; firmware
 *                        may still grant it later (a JOIN whose slot conflicts
 *                        with a held slot is queued, not rejected)
 * orphan_token/_bss       slots (by bss_idx) of such a late grant, held by
 *                        firmware but owned by no live request; released
 *                        under orphan_token at the next release point
 */
/* Grant incarnation: one firmware channel-privilege slot, keyed by
 * (token, bss_idx). The same logical link can have several incarnations
 * (the authenticated one carrying the MLD and a newly prepared one), and
 * they must never be confused: aborting the authenticated incarnation kills
 * its RX datapath irrecoverably (every host-side rebuild was tried).
 */
enum mt7925_mlo_grant_life {
	MT7925_MLO_GRANT_FREE,
	MT7925_MLO_GRANT_PREPARING,	/* requested / granted, not proven */
	MT7925_MLO_GRANT_HELD,		/* granted, not the continuity owner */
	MT7925_MLO_GRANT_HELD_CONTINUITY, /* security-valid, carries the MLD */
	MT7925_MLO_GRANT_VALIDATED,	/* new path with proven secure RX */
	MT7925_MLO_GRANT_OBSOLETE,	/* handed off, may be released */
	MT7925_MLO_GRANT_RELEASED,
	MT7925_MLO_GRANT_ORPHAN,	/* late grant of a cancelled request */
};

#define MT7925_MLO_GRANT_MAX	6

struct mt7925_mlo_grant_inc {
	unsigned int token;
	unsigned int link_id;
	unsigned int bss_idx;
	unsigned int dbdcband;
	unsigned int channel;
	unsigned int life;
	unsigned int continuity_owner;
	unsigned int secure_rx;
};

struct mt7925_mlo_roc_state {
	/* grant incarnations; at most one continuity_owner */
	struct mt7925_mlo_grant_inc grant[MT7925_MLO_GRANT_MAX];
	unsigned int held_token;
	unsigned int held_links;
	unsigned int pending_token;
	unsigned int pending_links;
	unsigned int timedout_token;
	unsigned int orphan_token;
	unsigned int orphan_bss;
	/* Token that owns each held link's slot. A delta acquisition grants
	 * the incoming link under its own fresh token while the survivor's
	 * slot stays under the token of the original JOIN, so one held_token
	 * cannot name every owner; aborts must use the slot's real owner.
	 * 0 means "the token held_token names".
	 */
	unsigned int link_token[3];
	/* Test-only same-token slot attach: one added link's slot requested
	 * under the HELD token. Kept apart from held_* so the survivor's
	 * ownership is never overwritten while waiting, and identified by
	 * token + bss_idx, never by token alone (held_token == attach token).
	 */
	unsigned int attach_pending_token;
	unsigned int attach_pending_links;
	unsigned int attach_pending_bss;
	unsigned int attach_timedout_token;
	unsigned int attach_timedout_bss;
	/* Single-link (delta) ACQUIRE transaction, also used by the test-only
	 * probe test_mt7927_mlo_roc_single_acquire: one fresh-token, one-TLV
	 * request for the added link only. Never touches the held and
	 * pending fields. state: 0 idle, 1 pending, 2 GRANT,
	 * 3 SUB_LINK event, 4 event with status != 0.
	 */
	unsigned int single_token;
	unsigned int single_state;
	unsigned int single_bss;
	unsigned int single_ev_bss;
	unsigned int single_status;
	unsigned int single_dbdc;
	unsigned int single_ch;
	unsigned int single_evt_ms;
	unsigned long single_t0;
	/* aborts refused by the continuity invariant (diagnostics) */
	unsigned int abort_blocked;
};

static inline void
mt7925_mlo_roc_state_request(struct mt7925_mlo_roc_state *st,
			     unsigned int token, unsigned int links)
{
	st->pending_token = token;
	st->pending_links = links;
	st->timedout_token = 0;
}

/* Matching firmware grant: pending becomes held. Returns 1 when slots of a
 * different token were still held (never observed; must not be silent).
 */
static inline unsigned int
mt7925_mlo_roc_state_grant(struct mt7925_mlo_roc_state *st)
{
	unsigned int anomaly = st->held_links &&
			       st->held_token != st->pending_token;
	unsigned int i;

	if (st->held_links && st->held_token == st->pending_token) {
		st->held_links |= st->pending_links;
	} else {
		st->held_links = st->pending_links;
		for (i = 0; i < 3; i++)
			st->link_token[i] = 0;
	}
	for (i = 0; i < 3; i++)
		if (st->pending_links & (1u << i))
			st->link_token[i] = st->pending_token;
	st->held_token = st->pending_token;
	st->pending_token = 0;
	st->pending_links = 0;
	return anomaly;
}

/* Request timed out: returns every slot the request named, to be aborted
 * under *token (the REQUEST's token). The held reservation keeps its own
 * token and slots; a queued grant is cancelled, not left to arrive later.
 * The token is remembered so a late grant can still be recognised.
 */
static inline unsigned int
mt7925_mlo_roc_state_timeout(struct mt7925_mlo_roc_state *st,
			     unsigned int *token)
{
	unsigned int mask = st->pending_links;

	*token = st->pending_token;
	st->timedout_token = st->pending_token;
	st->pending_token = 0;
	st->pending_links = 0;
	return mask;
}

/* A grant event for @token/@bss_idx. Returns 1 (and records the slot as an
 * orphan) when it belongs to the last timed-out request.
 */
static inline unsigned int
mt7925_mlo_roc_state_late_grant(struct mt7925_mlo_roc_state *st,
				unsigned int token, unsigned int bss_idx)
{
	if (!token || token != st->timedout_token)
		return 0;
	if (st->orphan_token != token) {
		st->orphan_token = token;
		st->orphan_bss = 0;
	}
	st->orphan_bss |= 1u << bss_idx;
	return 1;
}

/* Take the orphaned slots (bss_idx mask) and their token for release. */
static inline unsigned int
mt7925_mlo_roc_state_orphan_take(struct mt7925_mlo_roc_state *st,
				 unsigned int *token)
{
	unsigned int mask = st->orphan_bss;

	*token = st->orphan_token;
	st->orphan_bss = 0;
	return mask;
}

/* Token owning @link's slot (link_token, else the JOIN's held_token). */
static inline unsigned int
mt7925_mlo_roc_state_owner_token(const struct mt7925_mlo_roc_state *st,
				 unsigned int link)
{
	return link < 3 && st->link_token[link] ? st->link_token[link] :
						  st->held_token;
}

/* held_token names the owner of the lowest still-held link, and is dropped
 * when nothing is held, so traces stay meaningful after partial releases.
 */
static inline void
mt7925_mlo_roc_state_retarget(struct mt7925_mlo_roc_state *st)
{
	unsigned int i;

	if (!st->held_links) {
		st->held_token = 0;
		return;
	}
	for (i = 0; i < 3; i++) {
		if (st->held_links & (1u << i)) {
			st->held_token = mt7925_mlo_roc_state_owner_token(st, i);
			return;
		}
	}
}

/* A delta acquisition (one link, fresh token) was granted: record the slot
 * under its own token without touching the survivor's ownership.
 */
static inline unsigned int
mt7925_mlo_roc_state_delta_grant(struct mt7925_mlo_roc_state *st,
				 unsigned int token, unsigned int link)
{
	if (!token || link >= 3)
		return 0;
	st->link_token[link] = token;
	st->held_links |= 1u << link;
	if (!st->held_token)
		st->held_token = token;
	return 1;
}

/* Release one link's slot; *token is the token that owns it. */
static inline unsigned int
mt7925_mlo_roc_state_release_link(struct mt7925_mlo_roc_state *st,
				  unsigned int link, unsigned int *token)
{
	if (link >= 3 || !(st->held_links & (1u << link))) {
		*token = 0;
		return 0;
	}
	*token = mt7925_mlo_roc_state_owner_token(st, link);
	st->held_links &= ~(1u << link);
	st->link_token[link] = 0;
	mt7925_mlo_roc_state_retarget(st);
	return 1u << link;
}

/* Release held slots in @mask under the held token. Returns the slots that
 * were actually held. The token is dropped only when no slot remains.
 */
static inline unsigned int
mt7925_mlo_roc_state_release(struct mt7925_mlo_roc_state *st,
			     unsigned int mask)
{
	unsigned int rel = mask & st->held_links;
	unsigned int i;

	st->held_links &= ~rel;
	for (i = 0; i < 3; i++)
		if (rel & (1u << i))
			st->link_token[i] = 0;
	mt7925_mlo_roc_state_retarget(st);
	return rel;
}

/* Release everything held; returns the slots, *token the owning token. */
static inline unsigned int
mt7925_mlo_roc_state_release_all(struct mt7925_mlo_roc_state *st,
				 unsigned int *token)
{
	unsigned int rel = st->held_links;
	unsigned int i;

	*token = st->held_token;
	st->held_links = 0;
	st->held_token = 0;
	for (i = 0; i < 3; i++)
		st->link_token[i] = 0;
	for (i = 0; i < MT7925_MLO_GRANT_MAX; i++)
		st->grant[i] = (struct mt7925_mlo_grant_inc){};
	st->attach_pending_token = 0;
	st->attach_pending_links = 0;
	st->attach_pending_bss = 0;
	return rel;
}

static inline void
mt7925_mlo_roc_state_attach_request(struct mt7925_mlo_roc_state *st,
				    unsigned int token, unsigned int links,
				    unsigned int bss_mask)
{
	st->attach_pending_token = token;
	st->attach_pending_links = links;
	st->attach_pending_bss = bss_mask;
	st->attach_timedout_token = 0;
	st->attach_timedout_bss = 0;
}

/* Matching SUB_LINK/GRANT event: only token AND bss_idx of the pending
 * attach promote the added link into held_links.
 */
static inline unsigned int
mt7925_mlo_roc_state_attach_grant(struct mt7925_mlo_roc_state *st,
				  unsigned int token, unsigned int bss_idx)
{
	if (!st->attach_pending_token || token != st->attach_pending_token ||
	    !(st->attach_pending_bss & (1u << bss_idx)))
		return 0;

	st->held_links |= st->attach_pending_links;
	st->attach_pending_token = 0;
	st->attach_pending_links = 0;
	st->attach_pending_bss = 0;
	return 1;
}

/* Attach timed out: returns the added link(s) to cancel under *token; held
 * state (the survivor) is untouched. The identity is kept for a late grant.
 */
static inline unsigned int
mt7925_mlo_roc_state_attach_timeout(struct mt7925_mlo_roc_state *st,
				    unsigned int *token)
{
	unsigned int links = st->attach_pending_links;

	*token = st->attach_pending_token;
	st->attach_timedout_token = st->attach_pending_token;
	st->attach_timedout_bss = st->attach_pending_bss;
	st->attach_pending_token = 0;
	st->attach_pending_links = 0;
	st->attach_pending_bss = 0;
	return links;
}

/* Late grant for a timed-out attach: only the added bss slot is an orphan,
 * even though its token equals the held token.
 */
static inline unsigned int
mt7925_mlo_roc_state_attach_late_grant(struct mt7925_mlo_roc_state *st,
				       unsigned int token, unsigned int bss_idx)
{
	if (!token || token != st->attach_timedout_token ||
	    !(st->attach_timedout_bss & (1u << bss_idx)))
		return 0;
	if (st->orphan_token != token) {
		st->orphan_token = token;
		st->orphan_bss = 0;
	}
	st->orphan_bss |= 1u << bss_idx;
	return 1;
}

/* ---- grant incarnations and the continuity invariant ---- */

static inline int
mt7925_mlo_grant_find(const struct mt7925_mlo_roc_state *st,
		      unsigned int token, unsigned int bss_idx)
{
	int i;

	if (!token)
		return -1;
	for (i = 0; i < MT7925_MLO_GRANT_MAX; i++)
		if (st->grant[i].life != MT7925_MLO_GRANT_FREE &&
		    st->grant[i].token == token &&
		    st->grant[i].bss_idx == bss_idx)
			return i;
	return -1;
}

/* Record (or update) the incarnation (token, bss_idx). A full table recycles
 * a RELEASED/ORPHAN/OBSOLETE entry, never the continuity owner. Returns the
 * index, -1 when nothing could be recorded.
 */
static inline int
mt7925_mlo_grant_note(struct mt7925_mlo_roc_state *st, unsigned int token,
		      unsigned int link_id, unsigned int bss_idx,
		      unsigned int dbdcband, unsigned int channel,
		      unsigned int life)
{
	int i = mt7925_mlo_grant_find(st, token, bss_idx);

	if (!token)
		return -1;
	if (i < 0) {
		for (i = 0; i < MT7925_MLO_GRANT_MAX; i++)
			if (st->grant[i].life == MT7925_MLO_GRANT_FREE)
				break;
	}
	if (i >= MT7925_MLO_GRANT_MAX) {
		for (i = 0; i < MT7925_MLO_GRANT_MAX; i++)
			if (!st->grant[i].continuity_owner &&
			    (st->grant[i].life == MT7925_MLO_GRANT_RELEASED ||
			     st->grant[i].life == MT7925_MLO_GRANT_ORPHAN ||
			     st->grant[i].life == MT7925_MLO_GRANT_OBSOLETE))
				break;
		if (i >= MT7925_MLO_GRANT_MAX)
			return -1;
		st->grant[i] = (struct mt7925_mlo_grant_inc){};
	}
	st->grant[i].token = token;
	st->grant[i].link_id = link_id;
	st->grant[i].bss_idx = bss_idx;
	st->grant[i].dbdcband = dbdcband;
	st->grant[i].channel = channel;
	st->grant[i].life = life;
	return i;
}

/* Same-token two-TLV JOIN extension (diagnostic, default off): eligible
 * only when a token is actually held, the requested pair is a strict
 * superset of the held links, and exactly one link is being added -- i.e.
 * a legitimate "re-affirm the held slot as TLV0, request one more slot as
 * TLV1" shape, never a request that would also need to drop a held link
 * (nothing to re-affirm as TLV0 then) or add more than one link (not a
 * single extension). Returns the token to reuse, or 0 when not eligible.
 */
static inline unsigned int
mt7925_mlo_same_token_extend_eligible(const struct mt7925_mlo_roc_state *st,
				      unsigned int sel_links)
{
	if (!st->held_token || !st->held_links)
		return 0;
	if (st->held_links & ~sel_links)
		return 0;
	{
		unsigned int added = sel_links & ~st->held_links, n = 0, b;

		for (b = 0; b < 16; b++)
			if (added & (1u << b))
				n++;
		if (n != 1)
			return 0;
	}
	return st->held_token;
}

static inline int
mt7925_mlo_continuity_get(const struct mt7925_mlo_roc_state *st)
{
	int i;

	for (i = 0; i < MT7925_MLO_GRANT_MAX; i++)
		if (st->grant[i].life != MT7925_MLO_GRANT_FREE &&
		    st->grant[i].continuity_owner)
			return i;
	return -1;
}

/* Mark the held incarnation that carries the MLD as the continuity grant.
 * Only a security-valid grant becomes the owner; any previous owner reverts
 * to plain HELD. Returns the index.
 */
static inline int
mt7925_mlo_continuity_mark(struct mt7925_mlo_roc_state *st, unsigned int token,
			   unsigned int link_id, unsigned int bss_idx,
			   unsigned int dbdcband, unsigned int channel,
			   unsigned int secure_rx)
{
	int i, n;

	n = mt7925_mlo_grant_note(st, token, link_id, bss_idx, dbdcband,
				  channel, MT7925_MLO_GRANT_HELD);
	if (n < 0)
		return -1;
	st->grant[n].secure_rx = !!secure_rx;
	if (!secure_rx)
		return n;
	for (i = 0; i < MT7925_MLO_GRANT_MAX; i++) {
		if (i == n || !st->grant[i].continuity_owner)
			continue;
		st->grant[i].continuity_owner = 0;
		st->grant[i].life = MT7925_MLO_GRANT_HELD;
	}
	st->grant[n].continuity_owner = 1;
	st->grant[n].life = MT7925_MLO_GRANT_HELD_CONTINUITY;
	return n;
}

/* Record the secure-RX result of a prepared incarnation. */
static inline int
mt7925_mlo_grant_validate(struct mt7925_mlo_roc_state *st, unsigned int token,
			  unsigned int bss_idx, unsigned int secure_rx)
{
	int i = mt7925_mlo_grant_find(st, token, bss_idx);

	if (i < 0 || st->grant[i].continuity_owner)
		return -1;
	st->grant[i].secure_rx = !!secure_rx;
	st->grant[i].life = secure_rx ? MT7925_MLO_GRANT_VALIDATED :
					MT7925_MLO_GRANT_PREPARING;
	return i;
}

/* Another incarnation already carries security-valid RX. */
static inline int
mt7925_mlo_continuity_handoff_allowed(const struct mt7925_mlo_roc_state *st)
{
	int i;

	for (i = 0; i < MT7925_MLO_GRANT_MAX; i++)
		if (!st->grant[i].continuity_owner &&
		    st->grant[i].life == MT7925_MLO_GRANT_VALIDATED &&
		    st->grant[i].secure_rx)
			return 1;
	return 0;
}

/* Hand continuity ownership to the VALIDATED incarnation (token, bss_idx).
 * @keep_old: the old incarnation stays part of the final pair (delta
 * shape) and remains HELD; otherwise it becomes OBSOLETE (releasable).
 * Refused (-1) unless the target has proven security-valid RX.
 */
static inline int
mt7925_mlo_continuity_handoff(struct mt7925_mlo_roc_state *st,
			      unsigned int token, unsigned int bss_idx,
			      unsigned int keep_old)
{
	int n = mt7925_mlo_grant_find(st, token, bss_idx);
	int o = mt7925_mlo_continuity_get(st);

	if (n < 0 || st->grant[n].life != MT7925_MLO_GRANT_VALIDATED ||
	    !st->grant[n].secure_rx)
		return -1;
	if (o >= 0) {
		st->grant[o].continuity_owner = 0;
		st->grant[o].life = keep_old ? MT7925_MLO_GRANT_HELD :
					       MT7925_MLO_GRANT_OBSOLETE;
	}
	st->grant[n].continuity_owner = 1;
	st->grant[n].life = MT7925_MLO_GRANT_HELD_CONTINUITY;
	return n;
}

static inline void
mt7925_mlo_grant_released(struct mt7925_mlo_roc_state *st, unsigned int token,
			  unsigned int bss_idx)
{
	int i = mt7925_mlo_grant_find(st, token, bss_idx);

	if (i >= 0 && !st->grant[i].continuity_owner)
		st->grant[i].life = MT7925_MLO_GRANT_RELEASED;
}

enum mt7925_mlo_abort_verdict {
	MT7925_MLO_ABORT_ALLOW_UNTRACKED,
	MT7925_MLO_ABORT_ALLOW_NON_CONTINUITY,
	MT7925_MLO_ABORT_ALLOW_OBSOLETE,
	MT7925_MLO_ABORT_ALLOW_ORPHAN,
	MT7925_MLO_ABORT_ALLOW_TEARDOWN,
	MT7925_MLO_ABORT_ALLOW_HANDED_OFF,
	MT7925_MLO_ABORT_ALLOW_TEST_GATE,
	MT7925_MLO_ABORT_BLOCK_CONTINUITY,
};

static inline int mt7925_mlo_roc_abort_verdict_allows(int v)
{
	return v != MT7925_MLO_ABORT_BLOCK_CONTINUITY;
}

static inline const char *mt7925_mlo_roc_abort_verdict_name(int v)
{
	static const char * const n[] = {
		"untracked", "non_continuity", "obsolete", "orphan",
		"teardown", "handed_off", "test_gate",
		"last_security_valid_grant",
	};

	return (unsigned int)v <= MT7925_MLO_ABORT_BLOCK_CONTINUITY ? n[v] : "?";
}

/* THE continuity invariant: never release the last security-valid grant.
 * An abort of (token, bss_idx) is allowed only when the association is
 * being torn down (@assoc == 0), the slot is not the security-valid
 * continuity grant, another incarnation already carries security-valid RX,
 * or an explicit destructive test gate is enabled (@test_gate).
 */
static inline int
mt7925_mlo_roc_abort_check(const struct mt7925_mlo_roc_state *st,
			   unsigned int token, unsigned int bss_idx,
			   unsigned int assoc, unsigned int test_gate)
{
	int i;

	if (!assoc)
		return MT7925_MLO_ABORT_ALLOW_TEARDOWN;
	i = mt7925_mlo_grant_find(st, token, bss_idx);
	if (i < 0)
		return MT7925_MLO_ABORT_ALLOW_UNTRACKED;
	if (st->grant[i].continuity_owner && st->grant[i].secure_rx) {
		if (mt7925_mlo_continuity_handoff_allowed(st))
			return MT7925_MLO_ABORT_ALLOW_HANDED_OFF;
		if (test_gate)
			return MT7925_MLO_ABORT_ALLOW_TEST_GATE;
		return MT7925_MLO_ABORT_BLOCK_CONTINUITY;
	}
	if (st->grant[i].life == MT7925_MLO_GRANT_ORPHAN)
		return MT7925_MLO_ABORT_ALLOW_ORPHAN;
	if (st->grant[i].life == MT7925_MLO_GRANT_OBSOLETE)
		return MT7925_MLO_ABORT_ALLOW_OBSOLETE;
	return MT7925_MLO_ABORT_ALLOW_NON_CONTINUITY;
}

/* Filter a held-slot release mask (links) through the invariant, using each
 * slot's owner token and @bss_of_link. Returns the links that may go.
 */
static inline unsigned int
mt7925_mlo_roc_guard_mask(const struct mt7925_mlo_roc_state *st,
			  unsigned int mask, const unsigned int bss_of_link[3],
			  unsigned int assoc, unsigned int test_gate)
{
	unsigned int out = 0, i;

	for (i = 0; i < 3; i++) {
		if (!(mask & (1u << i)))
			continue;
		if (mt7925_mlo_roc_abort_verdict_allows(
			    mt7925_mlo_roc_abort_check(st,
				mt7925_mlo_roc_state_owner_token(st, i),
				bss_of_link[i], assoc, test_gate)))
			out |= 1u << i;
	}
	return out;
}

#endif
