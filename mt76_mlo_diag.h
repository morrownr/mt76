/* SPDX-License-Identifier: BSD-3-Clause-Clear */

#ifndef __MT76_MLO_DIAG_H
#define __MT76_MLO_DIAG_H

#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/icmp.h>
#include <linux/ip.h>

struct mt76_mlo_tx_select_trace {
	u32 hash;
	u16 protocol, orig_wcid, sel_wcid, l3_proto, arp_op, icmp_id, icmp_seq;
	u8 l4_proto, icmp_type, icmp_code;
	u8 path, priority, txq_tid, qid;
	u8 orig_link, orig_phy, sel_link, sel_phy, info_link;
	u8 da[ETH_ALEN];
	bool l4_hash, sw_hash, aggr, sta_present, vif_present;
};

struct mt76_mlo_txwi_trace {
	u32 key_cipher;
	u16 token, pid, wcid, wlan, wcid_cipher, seq;
	u16 l3_proto, arp_op, icmp_id, icmp_seq;
	s16 mconf_link, bss, omac, wmm, key_idx, hw_key_idx, ring;
	u8 link, sel_phy, txd_qidx, tgid, tid, qid, dma_phy;
	u8 l4_proto, icmp_type, icmp_code;
	bool protect, ba_disable, hw_amsdu, sn_valid;
};

struct mt76_mlo_pkt_sig {
	u16 l3_proto, arp_op, icmp_id, icmp_seq;
	u8 l4_proto, icmp_type, icmp_code;
};

static inline void mt76_mlo_skb_pkt_sig(struct sk_buff *skb,
					struct mt76_mlo_pkt_sig *sig)
{
	struct ethhdr _eh, *eh;

	memset(sig, 0, sizeof(*sig));

	eh = skb_header_pointer(skb, 0, sizeof(_eh), &_eh);
	if (!eh)
		return;

	sig->l3_proto = ntohs(eh->h_proto);
	if (sig->l3_proto == ETH_P_ARP) {
		struct arphdr _arp, *arp;

		arp = skb_header_pointer(skb, ETH_HLEN, sizeof(_arp), &_arp);
		if (arp)
			sig->arp_op = ntohs(arp->ar_op);
		return;
	}

	if (sig->l3_proto == ETH_P_IP) {
		struct icmphdr _icmp, *icmp;
		struct iphdr _ip, *ip;
		unsigned int off;

		ip = skb_header_pointer(skb, ETH_HLEN, sizeof(_ip), &_ip);
		if (!ip || ip->version != 4 || ip->ihl < 5)
			return;

		sig->l4_proto = ip->protocol;
		if (ip->protocol != IPPROTO_ICMP)
			return;

		off = ETH_HLEN + ip->ihl * 4;
		icmp = skb_header_pointer(skb, off, sizeof(_icmp), &_icmp);
		if (!icmp)
			return;

		sig->icmp_type = icmp->type;
		sig->icmp_code = icmp->code;
		sig->icmp_id = ntohs(icmp->un.echo.id);
		sig->icmp_seq = ntohs(icmp->un.echo.sequence);
	}
}

static inline void
mt76_mlo_tx_select_set_pkt_sig(struct mt76_mlo_tx_select_trace *v,
			       const struct mt76_mlo_pkt_sig *sig)
{
	v->l3_proto = sig->l3_proto;
	v->l4_proto = sig->l4_proto;
	v->arp_op = sig->arp_op;
	v->icmp_type = sig->icmp_type;
	v->icmp_code = sig->icmp_code;
	v->icmp_id = sig->icmp_id;
	v->icmp_seq = sig->icmp_seq;
}

static inline void
mt76_mlo_txwi_set_pkt_sig(struct mt76_mlo_txwi_trace *v,
			  const struct mt76_mlo_pkt_sig *sig)
{
	v->l3_proto = sig->l3_proto;
	v->l4_proto = sig->l4_proto;
	v->arp_op = sig->arp_op;
	v->icmp_type = sig->icmp_type;
	v->icmp_code = sig->icmp_code;
	v->icmp_id = sig->icmp_id;
	v->icmp_seq = sig->icmp_seq;
}

static inline bool mt76_mlo_pkt_sig_needs_txs(const struct mt76_mlo_pkt_sig *sig)
{
	return sig->l3_proto == ETH_P_ARP ||
	       (sig->l3_proto == ETH_P_IP && sig->l4_proto == IPPROTO_ICMP);
}

extern bool mt76_mlo_diag_trace;
extern u8 mt76_mlo_bssadd_skip;
extern u32 mt76_mlo_bssadd_delay_ms;
void mt76_mlo_bssinfo_deferred_send(void);

#endif /* __MT76_MLO_DIAG_H */
