// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Affiliated RX routing and secure-RX evidence execution owner.
 * Generic parsing, decrypt/PN handling and delivery remain dependencies.
 * Host RX observations do not establish new-path hardware acceptance.
 */
#include <linux/etherdevice.h>
#include "datapath.h"
#include "../mac.h"
#include "../mlo_valid_rx.h"
#include "../mlo_rx_band_resolve.h"
#include "../mlo_rx_freq_resolve.h"

/* EXPERIMENTAL (default N): during MLO link activation the firmware attributes
 * some frames that arrive on the anchor's radio to the freshly created partner
 * WCID. They carry valid CCMP and bump mlo_valid_rx_packets, so the partner's
 * fresh-admission RX proof passes with zero frames ever received on its own
 * channel. With this set, RX only counts as proof for a link when it arrived on
 * that link's operating channel.
 */
static bool mt7925_mlo_rx_chan_check;
module_param_named(mlo_rx_chan_check, mt7925_mlo_rx_chan_check, bool, 0644);
MODULE_PARM_DESC(mlo_rx_chan_check,
		 "EXPERIMENTAL: count valid-RX proof only when the frame arrived on the link's own channel (default N)");

/* Default Y: attribute a unicast frame to the affiliated link by the channel it was received on when the firmware's
 * WLAN_IDX names a different link. The firmware reports the MLD's primary WCID for frames of every link, so without this
 * a fresh 0x5 association delivers authenticated 2.4 GHz frames (5437 in a 20 s upload, key0, mac80211 core entry reached)
 * labelled WCID1/link2 instead of WCID2/link0. After a pair transition it also keeps naming the retired link for frames of
 * the incoming link, which mac80211 then drops on an inactive link. See mlo_rx_freq_resolve.h.
 */
static bool mt7925_mlo_rx_freq_attrib = true;
module_param_named(mlo_rx_freq_attrib, mt7925_mlo_rx_freq_attrib, bool, 0644);
MODULE_PARM_DESC(mlo_rx_freq_attrib,
		 "Attribute MT7927 MLD unicast RX to the link by receive channel (default Y)");

/* Default Y: the firmware names the MLD's first (primary) WCID in the RX descriptor for frames of every
 * link and the host re-attributes by BAND_IDX (mlo_rx_band_resolve.h). After a link replacement the retired link's WCID
 * still matches the band of its replacement and the candidates carry stale WCID bands. With this set a retired
 * resolved WCID counts as a mismatch, retired links are not candidates, and candidate bands come from the link's BSS.
 */
static bool mt7925_mlo_rx_retired_aware = true;
module_param_named(mlo_rx_retired_aware, mt7925_mlo_rx_retired_aware, bool, 0644);
MODULE_PARM_DESC(mlo_rx_retired_aware,
		 "MT7927 RX band attribution ignores retired links and uses BSS bands (default Y)");

static bool mt7925_mlo_rx_link_retired(struct mt792x_link_sta *mlink)
{
	struct mt792x_sta *msta = mlink->sta;
	struct ieee80211_vif *vif;

	if (!mlink->wcid.link_valid)
		return true;
	if (!msta || !msta->vif)
		return false;

	vif = container_of((void *)msta->vif, struct ieee80211_vif, drv_priv);
	return !(vif->active_links & BIT(mlink->wcid.link_id));
}

static int
mt7925_mlo_rx_wrong_channel(struct mt792x_link_sta *mlink, int rx_freq)
{
	struct ieee80211_bss_conf *link_conf;
	struct ieee80211_vif *vif;
	struct mt792x_sta *msta = mlink->sta;
	unsigned int link_id = mlink->wcid.link_id;
	int wrong = 0;

	if (!READ_ONCE(mt7925_mlo_rx_chan_check) || !rx_freq || !msta ||
	    !msta->vif || link_id >= IEEE80211_MLD_MAX_NUM_LINKS)
		return 0;

	vif = container_of((void *)msta->vif, struct ieee80211_vif, drv_priv);
	rcu_read_lock();
	link_conf = rcu_dereference(vif->link_conf[link_id]);
	if (link_conf && link_conf->chanreq.oper.chan &&
	    link_conf->chanreq.oper.chan->center_freq != rx_freq)
		wrong = link_conf->chanreq.oper.chan->center_freq;
	rcu_read_unlock();

	if (wrong && (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
		pr_info_ratelimited("MLO_RX_WRONG_CHAN: link=%u wcid=%u rx_freq=%d link_freq=%d\n",
				    link_id, mlink->wcid.idx, rx_freq, wrong);
	return !!wrong;
}

static void
mt7925_mlo_account_valid_rx(struct mt792x_link_sta *mlink,
			    const struct mt7925_mlo_rx_evidence *ev)
{
	struct mt7925_mlo_rearm_state rs = {};
	struct ieee80211_vif *vif;
	struct mt792x_sta *msta = mlink->sta;
	struct mt792x_vif *mvif;
	unsigned int link_id = mlink->wcid.link_id;

	if (!mt7925_mlo_rx_is_valid_datapath(ev))
		return;

	mlink->mlo_valid_rx_packets++;
	WRITE_ONCE(mlink->mlo_valid_rx_jiffies, jiffies);

	if (!msta || !msta->vif || link_id >= IEEE80211_MLD_MAX_NUM_LINKS)
		return;

	mvif = msta->vif;
	vif = container_of((void *)mvif, struct ieee80211_vif, drv_priv);

	rs.link_valid = mlink->wcid.link_valid;
	rs.in_active_links = !!(vif->active_links & BIT(link_id));
	rs.awaiting_valid_rx = READ_ONCE(mlink->mlo_await_valid_rx);
	rs.await_generation = READ_ONCE(mlink->mlo_await_generation);
	rs.cur_generation = READ_ONCE(mvif->mt7927_mlo.transition_generation);
	rs.ready_ts = READ_ONCE(msta->mlo_link_ready_jiffies[link_id]);

	if (!mt7925_mlo_should_rearm_on_valid_rx(&rs, 1))
		return;

	WRITE_ONCE(mlink->mlo_await_valid_rx, false);
	WRITE_ONCE(msta->mlo_link_ready_jiffies[link_id], jiffies);
	if (READ_ONCE(mt76_mlo_diag_trace))
		printk(KERN_INFO
		       "MLO_LINK_TX_READY_VALID_RX: link=%u wcid=%u generation=%u valid_rx=%u\n",
		       link_id, mlink->wcid.idx, rs.cur_generation,
		       mlink->mlo_valid_rx_packets);
}

static void
mt7925_mlo_rx_route_band(struct mt792x_dev *dev,
			 struct mt76_rx_status *status, bool unicast,
			 u16 idx, u8 desc_band_idx)
{
	struct mt792x_link_sta *mlink;

	if (!status->wcid)
		return;
	mlink = container_of(status->wcid, struct mt792x_link_sta, wcid);

	/* MT7927 dual-radio RX WLAN_IDX ambiguity (see mlo_rx_band_resolve.h):
	 * the raw fw_idx above can be scoped to the receiving radio's local
	 * peer table instead of the host's globally-unique STA_REC index.
	 * Cross-check the resolved wcid's own band against the descriptor's
	 * BAND_IDX and re-attribute to the affiliated link that actually
	 * owns that band when they disagree.
	 */
	if (unicast && mlink) {
		struct mt7925_mlo_rx_band_ctx bctx = {
			.has_wcid = 1,
			.wcid_phy_idx = status->wcid->phy_idx,
			.desc_band_idx = desc_band_idx,
			.wcid_retired = READ_ONCE(mt7925_mlo_rx_retired_aware) &&
					is_mt7927(&dev->mt76) &&
					mt7925_mlo_rx_link_retired(mlink),
		};

		/* WCID phy_idx can retain the creation-time radio. Use the
		 * member's programmed BSS band for the initial check too, not
		 * only when selecting an affiliated replacement below.
		 */
		if (READ_ONCE(mt7925_mlo_rx_retired_aware) &&
		    is_mt7927(&dev->mt76) && mlink->sta &&
		    mlink->sta->vif &&
		    status->wcid->link_id < IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_bss_conf *cm =
				rcu_dereference(mlink->sta->vif->link_conf[status->wcid->link_id]);

			if (cm)
				bctx.wcid_phy_idx = cm->mt76.band_idx;
		}

		if (mt7925_mlo_rx_band_mismatch(&bctx)) {
			struct mt7925_mlo_rx_band_candidate cands[IEEE80211_MLD_MAX_NUM_LINKS] = {};
			struct mt792x_sta *msta = mlink->sta;
			struct mt792x_link_sta *remap = NULL;
			int sel;
			unsigned int lid;

			if (msta) {
				for (lid = 0; lid < IEEE80211_MLD_MAX_NUM_LINKS; lid++) {
					struct mt792x_link_sta *cand;

					if (!(msta->valid_links & BIT(lid)))
						continue;

					cand = rcu_dereference(msta->link[lid]);
					if (!cand)
						continue;

					cands[lid].present = 1;
					cands[lid].phy_idx = cand->wcid.phy_idx;
					if (READ_ONCE(mt7925_mlo_rx_retired_aware) &&
					    is_mt7927(&dev->mt76) && msta->vif) {
						struct mt792x_bss_conf *cm =
							rcu_dereference(msta->vif->link_conf[lid]);

						cands[lid].present = !mt7925_mlo_rx_link_retired(cand);
						if (cm)
							cands[lid].phy_idx = cm->mt76.band_idx;
					}
				}

				sel = mt7925_mlo_rx_band_select(cands, IEEE80211_MLD_MAX_NUM_LINKS,
								desc_band_idx);
				if (sel >= 0)
					remap = rcu_dereference(msta->link[sel]);
			}

			if (unlikely(READ_ONCE(mt76_mlo_diag_trace)))
				printk(KERN_INFO
				      "MLO_RX_BAND_REMAP: fw_idx=%u desc_band=%u wcid_band=%u from_link=%d to_link=%d\n",
				      idx, desc_band_idx, status->wcid->phy_idx,
				      mlink->wcid.link_id,
				      remap ? remap->wcid.link_id : -1);

			if (remap) {
				status->wcid = &remap->wcid;
				mlink = remap;
			}
		}
	}

}

/* MLO_RX_DIAG_BEGIN */
/* Private metadata only: leave shared trace ABI and RX skb/validation intact. */


void
mt7925_mlo_rx_diag_parse(struct sk_buff *skb, u32 flags, u8 sec_mode,
			bool amsdu, struct mt7925_mlo_rx_diag *d)
{
	u8 scratch[36], *p;
	struct ieee80211_hdr *hdr;
	unsigned int off, end, hlen, iplen, tags = 0;
	u16 proto, qos = 0;
	__le16 fc;
	bool snap = false;

	memset(d, 0, sizeof(*d));
	end = skb->len;
	if (flags & RX_FLAG_8023) {
		p = skb_header_pointer(skb, 0, ETH_HLEN, scratch);
		if (!p)
			goto short_frame;
		ether_addr_copy(d->da, p);
		ether_addr_copy(d->sa, p + ETH_ALEN);
		d->mac_valid = true;
		proto = get_unaligned_be16(p + 12);
		off = ETH_HLEN;
		while (proto == ETH_P_8021Q || proto == ETH_P_8021AD) {
			if (++tags > 2)
				return;
			p = skb_header_pointer(skb, off, 4, scratch);
			if (!p)
				goto short_frame;
			proto = get_unaligned_be16(p + 2);
			off += 4;
		}
		snap = proto <= ETH_DATA_LEN;
		if (snap) {
			d->truncated = proto > skb->len - off;
			end = off + min_t(unsigned int, proto, skb->len - off);
		}
	} else {
		p = skb_header_pointer(skb, 0, 2, scratch);
		if (!p)
			goto short_frame;
		memcpy(&fc, p, sizeof(fc));
		d->fc = le16_to_cpu(fc);
		if (d->fc & IEEE80211_FCTL_VERS || !ieee80211_is_data_present(fc))
			return;
		hlen = ieee80211_hdrlen(fc);
		if (hlen > sizeof(scratch))
			return;
		p = skb_header_pointer(skb, 0, hlen, scratch);
		if (!p)
			goto short_frame;
		hdr = (void *)p;
		ether_addr_copy(d->sa, ieee80211_get_SA(hdr));
		ether_addr_copy(d->da, ieee80211_get_DA(hdr));
		ether_addr_copy(d->ra, hdr->addr1);
		ether_addr_copy(d->ta, hdr->addr2);
		d->seq_ctrl = le16_to_cpu(hdr->seq_ctrl);
		d->mac_valid = true;
		if (ieee80211_is_data_qos(fc)) {
			qos = p[ieee80211_has_a4(fc) ? 30 : 24] |
			      (p[ieee80211_has_a4(fc) ? 31 : 25] << 8);
			d->tid = qos & 0xf;
		}
		/* Aggregates/fragments/mesh need their own envelope/reassembly. */
		if (ieee80211_has_morefrags(fc) || d->seq_ctrl & IEEE80211_SCTL_FRAG ||
		    qos & IEEE80211_QOS_CTL_A_MSDU_PRESENT ||
		    (ieee80211_has_a4(fc) && qos & IEEE80211_QOS_CTL_MESH_CONTROL_PRESENT))
			return;
		off = hlen;
		if (ieee80211_has_protected(fc) && !(flags & RX_FLAG_IV_STRIPPED)) {
			/* Only the supported eight-byte CCMP/GCMP envelope, never its PN. */
			switch (sec_mode) {
			case MT_CIPHER_AES_CCMP:
			case MT_CIPHER_CCMP_CCX:
			case MT_CIPHER_CCMP_256:
			case MT_CIPHER_GCMP:
			case MT_CIPHER_GCMP_256:
				off += 8;
				break;
			default:
				return;
			}
		}
		snap = true;
		proto = 0;
		if (ieee80211_has_protected(fc) && !(flags & RX_FLAG_DECRYPTED))
			return;
	}
	if (amsdu || flags & (RX_FLAG_ONLY_MONITOR | RX_FLAG_FAILED_FCS_CRC | RX_FLAG_MMIC_ERROR) ||
	    (sec_mode && !(flags & RX_FLAG_DECRYPTED)))
		return;
	if (snap) {
		if (end - off < 8)
			goto short_frame;
		p = skb_header_pointer(skb, off, 8, scratch);
		if (!p)
			goto short_frame;
		/* RFC1042 or bridge-tunnel SNAP; no guessed proprietary envelope. */
		if (p[0] != 0xaa || p[1] != 0xaa || p[2] != 3 ||
		    p[3] || p[4] || (p[5] != 0 && p[5] != 0xf8))
			return;
		proto = get_unaligned_be16(p + 6);
		off += 8;
	}
	d->sig.l3_proto = proto;
	if (proto == ETH_P_ARP) {
		if (end - off < 8)
			goto short_frame;
		p = skb_header_pointer(skb, off, 8, scratch);
		if (!p)
			goto short_frame;
		d->sig.arp_op = get_unaligned_be16(p + 6);
		return;
	}
	if (proto != ETH_P_IP)
		return;
	if (end - off < 20)
		goto short_frame;
	p = skb_header_pointer(skb, off, 20, scratch);
	if (!p)
		goto short_frame;
	hlen = (p[0] & 0xf) * 4;
	iplen = get_unaligned_be16(p + 2);
	if (p[0] >> 4 != 4 || hlen < 20 || iplen < hlen)
		return;
	if (hlen > end - off)
		goto short_frame;
	d->truncated |= iplen > end - off;
	end = off + min_t(unsigned int, iplen, end - off);
	d->ip_id = get_unaligned_be16(p + 4);
	memcpy(&d->src_ip, p + 12, 4);
	memcpy(&d->dst_ip, p + 16, 4);
	d->ip_valid = true;
	d->sig.l4_proto = p[9];
	if (get_unaligned_be16(p + 6) & 0x3fff)
		return;
	off += hlen;
	if (d->sig.l4_proto == IPPROTO_ICMP) {
		if (end - off < 8)
			goto short_frame;
		p = skb_header_pointer(skb, off, 8, scratch);
		if (!p)
			goto short_frame;
		d->sig.icmp_type = p[0];
		d->sig.icmp_code = p[1];
		if (p[0] != ICMP_ECHO && p[0] != ICMP_ECHOREPLY)
			return;
		d->sig.icmp_id = get_unaligned_be16(p + 4);
		d->sig.icmp_seq = get_unaligned_be16(p + 6);
	} else if (d->sig.l4_proto == IPPROTO_TCP) {
		if (end - off < 20)
			goto short_frame;
		p = skb_header_pointer(skb, off, 20, scratch);
		if (!p)
			goto short_frame;
		hlen = (p[12] >> 4) * 4;
		if (hlen < 20)
			return;
		if (hlen > end - off)
			goto short_frame;
		d->sport = get_unaligned_be16(p);
		d->dport = get_unaligned_be16(p + 2);
		d->tcp_seq = get_unaligned_be32(p + 4);
		d->tcp_ack = get_unaligned_be32(p + 8);
	} else {
		return;
	}
	d->l4_valid = true;
	return;
short_frame:
	d->truncated = true;
}
/* MLO_RX_DIAG_END */

static bool mt7925_mlo_eapol_token_pending(struct mt76_dev *mdev)
{
	struct mt76_txwi_cache *txwi;
	bool pending;

	spin_lock_bh(&mdev->token_lock);
	txwi = idr_find(&mdev->token, 0);
	pending = txwi && txwi->mlo_diag_eapol;
	spin_unlock_bh(&mdev->token_lock);

	return pending;
}

void mt7925_mlo_raw_completion(struct mt792x_dev *dev,
				      enum rx_pkt_type type,
				      const void *data, int len)
{
	if (!unlikely(mt76_mlo_diag_trace) ||
	    !mt7925_mlo_eapol_token_pending(&dev->mt76) ||
	    (type != PKT_TYPE_TXRX_NOTIFY && type != PKT_TYPE_TXS))
		return;

	printk(KERN_INFO "MLO_EAPOL_RAW_RX_BOUNDARY: token=0 type=%u len=%d\n",
	       type, len);
	print_hex_dump(KERN_INFO, "MLO_EAPOL_RAW_RX_BOUNDARY: ",
		       DUMP_PREFIX_OFFSET, 16, 1, data, min_t(int, len, 64),
		       false);
}


static void
mt7925_mlo_rx_route_freq(struct mt792x_dev *dev,
			 struct mt76_rx_status *status, bool unicast,
			 u16 idx)
{
	struct mt792x_link_sta *mlink;

	if (!status->wcid)
		return;
	mlink = container_of(status->wcid, struct mt792x_link_sta, wcid);


	if (unicast && status->wcid && READ_ONCE(mt7925_mlo_rx_freq_attrib) &&
	    is_mt7927(&dev->mt76) && mlink->sta && mlink->sta->vif) {
		struct mt7925_mlo_rx_freq_cand cands[IEEE80211_MLD_MAX_NUM_LINKS] = {};
		struct mt792x_sta *msta = mlink->sta;
		struct ieee80211_vif *rvif = container_of((void *)msta->vif, struct ieee80211_vif, drv_priv);
		struct mt792x_link_sta *remap = NULL;
		unsigned int lid;
		int sel;

		rcu_read_lock();
		for (lid = 0; lid < IEEE80211_MLD_MAX_NUM_LINKS; lid++) {
			struct mt792x_link_sta *cand = rcu_dereference(msta->link[lid]);
			struct ieee80211_bss_conf *lc = rcu_dereference(rvif->link_conf[lid]);

			if (!cand || !lc || !lc->chanreq.oper.chan)
				continue;
			cands[lid].usable = cand->wcid.link_valid && (rvif->active_links & BIT(lid));
			cands[lid].freq = lc->chanreq.oper.chan->center_freq;
		}
		sel = mt7925_mlo_rx_freq_select(cands, IEEE80211_MLD_MAX_NUM_LINKS, status->freq,
						mlink->wcid.link_id);
		if (sel >= 0 && sel != mlink->wcid.link_id)
			remap = rcu_dereference(msta->link[sel]);
		rcu_read_unlock();

		if (remap) {
			if (unlikely(READ_ONCE(mt76_mlo_diag_trace)))
				printk_ratelimited(KERN_INFO
				       "MLO_RX_FREQ_REMAP: fw_idx=%u rx_freq=%d from_link=%d wcid=%u to_link=%d wcid=%u\n",
				       idx, status->freq, mlink->wcid.link_id, mlink->wcid.idx,
				       remap->wcid.link_id, remap->wcid.idx);
			status->wcid = &remap->wcid;
			mlink = remap;
		}
	}

}

void mt7925_mlo_rx_route(struct mt792x_dev *dev,
			 struct mt76_rx_status *status, bool unicast,
			 u16 fw_idx, u8 desc_band_idx)
{
	mt7925_mlo_rx_route_band(dev, status, unicast, fw_idx, desc_band_idx);
	mt7925_mlo_rx_route_freq(dev, status, unicast, fw_idx);
}

void mt7925_mlo_rx_account(struct sk_buff *skb, bool unicast, __le16 fc,
			   u32 rxd1, u32 rxd2, u32 rxd3)
{
	struct mt76_rx_status *status = (struct mt76_rx_status *)skb->cb;
	struct mt792x_link_sta *mlink;

	if (!status->wcid || !unicast)
		return;
	mlink = container_of(status->wcid, struct mt792x_link_sta, wcid);
	if (status->wcid && unicast) {
		struct mt7925_mlo_rx_evidence ev = {
			.has_wcid = 1,
			.unicast = 1,
			.is_data = (status->flag & RX_FLAG_8023) ||
				   ieee80211_is_data(fc),
			.is_null_data = !(status->flag & RX_FLAG_8023) &&
					(ieee80211_is_nullfunc(fc) ||
					 ieee80211_is_qos_nullfunc(fc)),
			.cm = !!(rxd1 & MT_RXD1_NORMAL_CM),
			.clm = !!(rxd1 & MT_RXD1_NORMAL_CLM),
			.icv_err = !!(rxd1 & MT_RXD1_NORMAL_ICV_ERR),
			.fcs_err = !!(rxd3 & MT_RXD3_NORMAL_FCS_ERR),
			.tkip_mic_err = !!(rxd1 & MT_RXD1_NORMAL_TKIP_MIC_ERR),
			.protected = FIELD_GET(MT_RXD2_NORMAL_SEC_MODE, rxd2) != 0,
			.decrypted = !!(status->flag & RX_FLAG_DECRYPTED),
			.peer_keyed = status->wcid->hw_key_idx != (u8)-1,
			.wrong_channel = mt7925_mlo_rx_wrong_channel(mlink,
								     status->freq),
		};

		mt7925_mlo_account_valid_rx(mlink, &ev);
	}

}
