/* MT7927 dual-radio RX WLAN_IDX ambiguity.
 *
 * The RX descriptor's WLAN_IDX field (MT_RXD1_NORMAL_WLAN_IDX) is expected
 * to be the same globally-unique index the host assigned via STA_REC/WTBL
 * CREATE. Live traces on this hardware show that is not always true: for a
 * frame received on band N, WLAN_IDX can instead reflect a value scoped to
 * band N's own local peer table, colliding with the (different) global
 * index legitimately owned by a peer on a different band. The result is a
 * frame's affiliated link getting silently misattributed to the wrong MLO
 * link -- decrypted/reordered against the wrong WCID/key/BA state, while
 * the real per-link RX for the misattributed band never surfaces.
 *
 * The descriptor also carries BAND_IDX (MT_RXD1_NORMAL_BAND_IDX): which
 * radio actually received the frame. That value stays trustworthy
 * regardless of the WLAN_IDX ambiguity above. So: resolve WLAN_IDX to a
 * wcid as usual, then cross-check its own band (wcid->phy_idx) against the
 * descriptor's BAND_IDX. A mismatch means the raw WLAN_IDX lookup cannot be
 * trusted for this frame, and the frame must instead be attributed to
 * whichever affiliated link of the same peer actually owns BAND_IDX --
 * unambiguous for a client-mode STA, which has exactly one peer (the AP)
 * per active band.
 */
#ifndef MT7925_MLO_RX_BAND_RESOLVE_H
#define MT7925_MLO_RX_BAND_RESOLVE_H

struct mt7925_mlo_rx_band_ctx {
	int has_wcid;
	unsigned int wcid_phy_idx;
	unsigned int desc_band_idx;
	/* The resolved WCID belongs to a retired link (link_valid = 0 or link not active). Its band can still equal the
	 * descriptor's band when the incoming link took over the retired link's firmware band slot; the frame then must
	 * not stay on it. Zero-initialised callers keep the stock behaviour.
	 */
	int wcid_retired;
};

static inline int
mt7925_mlo_rx_band_mismatch(const struct mt7925_mlo_rx_band_ctx *ctx)
{
	if (!ctx->has_wcid)
		return 0;

	if (ctx->wcid_retired)
		return 1;

	return ctx->wcid_phy_idx != ctx->desc_band_idx;
}

/* One candidate per affiliated link (any index order/count the caller
 * likes -- typically IEEE80211_MLD_MAX_NUM_LINKS slots, some unpopulated).
 * present=0 means "this link slot doesn't structurally exist right now"
 * (unused index, or a link mid removal) and must be skipped.
 */
struct mt7925_mlo_rx_band_candidate {
	int present;
	unsigned int phy_idx;
};

/* Return the index of the one candidate whose band matches desc_band_idx,
 * or -1 if none match or more than one matches. More than one match on the
 * same band should never happen for a client-mode STA (one peer per band)
 * -- treating it as -1 rather than guessing keeps a modeling bug from
 * silently misrouting a frame a second way instead of just leaving the
 * original (already known ambiguous) resolution in place.
 */
static inline int
mt7925_mlo_rx_band_select(const struct mt7925_mlo_rx_band_candidate *cands,
			  unsigned int n, unsigned int desc_band_idx)
{
	int found = -1;
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (!cands[i].present || cands[i].phy_idx != desc_band_idx)
			continue;
		if (found >= 0)
			return -1;
		found = (int)i;
	}

	return found;
}

#endif
