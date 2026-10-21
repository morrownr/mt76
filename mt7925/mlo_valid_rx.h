/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7925_MLO_VALID_RX_H
#define __MT7925_MLO_VALID_RX_H

/* Pure decision logic for MLO datapath RX evidence. No kernel headers so it
 * can be exercised by test_mlo_valid_rx.c in userspace.
 *
 * wcid.stats.rx_packets counts every descriptor that resolves to a WCID,
 * including cipher-mismatch and ICV-failed frames that never reach the
 * stack. Readiness, liveness and steering validation need proof that the
 * link delivers usable unicast data, so they read a separate counter fed
 * only by frames accepted here.
 */

struct mt7925_mlo_rx_evidence {
	int has_wcid;
	int unicast;
	int is_data;		/* 802.3 hdr-trans frame, or 802.11 data */
	int is_null_data;	/* (QoS) null function: no payload */
	int cm;			/* cipher mismatch */
	int clm;		/* cipher length mismatch */
	int icv_err;
	int fcs_err;
	int tkip_mic_err;
	int protected;		/* firmware sec_mode != 0 */
	int decrypted;
	int peer_keyed;		/* pairwise key installed for this link */
	int wrong_channel;	/* RX arrived on a channel other than this link's operating channel */
};

static inline int
mt7925_mlo_rx_is_valid_datapath(const struct mt7925_mlo_rx_evidence *e)
{
	if (!e->has_wcid || !e->unicast)
		return 0;
	/* The firmware can attribute a frame to another link's WCID while it
	 * arrives on this link's sibling radio; that says nothing about this
	 * link's own datapath.
	 */
	if (e->wrong_channel)
		return 0;
	if (!e->is_data || e->is_null_data)
		return 0;
	if (e->cm || e->clm || e->icv_err || e->fcs_err || e->tkip_mic_err)
		return 0;
	if (e->protected)
		return e->decrypted;
	/* Unprotected data on a keyed peer would be dropped by mac80211. */
	return !e->peer_keyed;
}

/* Late valid-RX rearm: only a link that is firmware-live, structurally
 * valid, still in the active topology, explicitly waiting for RX proof in
 * the current transition generation, and not yet ready may be rearmed.
 */
struct mt7925_mlo_rearm_state {
	int link_valid;
	int in_active_links;
	int awaiting_valid_rx;
	unsigned int await_generation;
	unsigned int cur_generation;
	unsigned long ready_ts;
};

static inline int
mt7925_mlo_should_rearm_on_valid_rx(const struct mt7925_mlo_rearm_state *s,
				    int rx_valid)
{
	if (!rx_valid)
		return 0;
	if (s->ready_ts)
		return 0;
	if (!s->link_valid || !s->in_active_links || !s->awaiting_valid_rx)
		return 0;
	return s->await_generation == s->cur_generation;
}

#endif
