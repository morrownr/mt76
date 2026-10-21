/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 */

#if !defined(__MT76_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define __MT76_TRACE_H

#include <linux/tracepoint.h>
#include "mt76.h"

#undef TRACE_SYSTEM
#define TRACE_SYSTEM mt76

#define MAXNAME		32
#define DEV_ENTRY	__array(char, wiphy_name, 32)
#define DEVICE_ASSIGN	strscpy(__entry->wiphy_name,	\
				wiphy_name(dev->hw->wiphy), MAXNAME)
#define DEV_PR_FMT	"%s"
#define DEV_PR_ARG	__entry->wiphy_name

#define REG_ENTRY	__field(u32, reg) __field(u32, val)
#define REG_ASSIGN	__entry->reg = reg; __entry->val = val
#define REG_PR_FMT	" %04x=%08x"
#define REG_PR_ARG	__entry->reg, __entry->val

#define TXID_ENTRY	__field(u8, wcid) __field(u8, pktid)
#define TXID_ASSIGN	__entry->wcid = wcid; __entry->pktid = pktid
#define TXID_PR_FMT	" [%d:%d]"
#define TXID_PR_ARG	__entry->wcid, __entry->pktid

DECLARE_EVENT_CLASS(dev_reg_evt,
	TP_PROTO(struct mt76_dev *dev, u32 reg, u32 val),
	TP_ARGS(dev, reg, val),
	TP_STRUCT__entry(
		DEV_ENTRY
		REG_ENTRY
	),
	TP_fast_assign(
		DEVICE_ASSIGN;
		REG_ASSIGN;
	),
	TP_printk(
		DEV_PR_FMT REG_PR_FMT,
		DEV_PR_ARG, REG_PR_ARG
	)
);

DEFINE_EVENT(dev_reg_evt, reg_rr,
	TP_PROTO(struct mt76_dev *dev, u32 reg, u32 val),
	TP_ARGS(dev, reg, val)
);

DEFINE_EVENT(dev_reg_evt, reg_wr,
	TP_PROTO(struct mt76_dev *dev, u32 reg, u32 val),
	TP_ARGS(dev, reg, val)
);

TRACE_EVENT(dev_irq,
	TP_PROTO(struct mt76_dev *dev, u32 val, u32 mask),

	TP_ARGS(dev, val, mask),

	TP_STRUCT__entry(
		DEV_ENTRY
		__field(u32, val)
		__field(u32, mask)
	),

	TP_fast_assign(
		DEVICE_ASSIGN;
		__entry->val = val;
		__entry->mask = mask;
	),

	TP_printk(
		DEV_PR_FMT " %08x & %08x",
		DEV_PR_ARG, __entry->val, __entry->mask
	)
);

DECLARE_EVENT_CLASS(dev_txid_evt,
	TP_PROTO(struct mt76_dev *dev, u8 wcid, u8 pktid),
	TP_ARGS(dev, wcid, pktid),
	TP_STRUCT__entry(
		DEV_ENTRY
		TXID_ENTRY
	),
	TP_fast_assign(
		DEVICE_ASSIGN;
		TXID_ASSIGN;
	),
	TP_printk(
		DEV_PR_FMT TXID_PR_FMT,
		DEV_PR_ARG, TXID_PR_ARG
	)
);

DEFINE_EVENT(dev_txid_evt, mac_txdone,
	TP_PROTO(struct mt76_dev *dev, u8 wcid, u8 pktid),
	TP_ARGS(dev, wcid, pktid)
);


TRACE_EVENT(mlo_tx_select,
	TP_PROTO(struct mt76_dev *dev, const struct mt76_mlo_tx_select_trace *v),
	TP_ARGS(dev, v),
		TP_STRUCT__entry(
			DEV_ENTRY
			__field(u8, path) __field(u32, hash)
			__field(bool, l4_hash) __field(bool, sw_hash)
			__field(u16, protocol) __array(u8, da, ETH_ALEN)
			__field(u16, l3_proto) __field(u8, l4_proto)
			__field(u16, arp_op) __field(u8, icmp_type)
			__field(u8, icmp_code) __field(u16, icmp_id)
			__field(u16, icmp_seq)
			__field(u8, priority) __field(u8, txq_tid) __field(u8, qid)
			__field(bool, aggr) __field(u16, orig_wcid)
			__field(u8, orig_link) __field(u8, orig_phy)
		__field(u16, sel_wcid) __field(u8, sel_link)
		__field(u8, sel_phy) __field(bool, sta_present)
		__field(bool, vif_present) __field(u8, info_link)
	),
	TP_fast_assign(
		DEVICE_ASSIGN;
		__entry->path = v->path; __entry->hash = v->hash;
			__entry->l4_hash = v->l4_hash; __entry->sw_hash = v->sw_hash;
			__entry->protocol = v->protocol; memcpy(__entry->da, v->da, ETH_ALEN);
			__entry->l3_proto = v->l3_proto; __entry->l4_proto = v->l4_proto;
			__entry->arp_op = v->arp_op; __entry->icmp_type = v->icmp_type;
			__entry->icmp_code = v->icmp_code; __entry->icmp_id = v->icmp_id;
			__entry->icmp_seq = v->icmp_seq;
			__entry->priority = v->priority; __entry->txq_tid = v->txq_tid;
			__entry->qid = v->qid; __entry->aggr = v->aggr;
			__entry->orig_wcid = v->orig_wcid; __entry->orig_link = v->orig_link;
		__entry->orig_phy = v->orig_phy; __entry->sel_wcid = v->sel_wcid;
		__entry->sel_link = v->sel_link; __entry->sel_phy = v->sel_phy;
		__entry->sta_present = v->sta_present; __entry->vif_present = v->vif_present;
		__entry->info_link = v->info_link;
	),
		TP_printk(DEV_PR_FMT " path=%u hash=%08x l4=%u sw=%u proto=0x%04x da=%pM l3=0x%04x l4p=%u arp_op=%u icmp=%u/%u id=%u seq=%u pri=%u txq_tid=%u qid=%u aggr=%u orig=%u/%u/%u sel=%u/%u/%u sta=%u vif=%u info_link=%u",
			DEV_PR_ARG, __entry->path, __entry->hash, __entry->l4_hash,
			__entry->sw_hash, __entry->protocol, __entry->da,
			__entry->l3_proto, __entry->l4_proto, __entry->arp_op,
			__entry->icmp_type, __entry->icmp_code, __entry->icmp_id,
			__entry->icmp_seq,
			__entry->priority, __entry->txq_tid, __entry->qid,
			__entry->aggr, __entry->orig_wcid, __entry->orig_link,
			__entry->orig_phy, __entry->sel_wcid, __entry->sel_link,
		__entry->sel_phy, __entry->sta_present, __entry->vif_present,
		__entry->info_link)
);

TRACE_EVENT(mlo_txwi,
	TP_PROTO(struct mt76_dev *dev, const struct mt76_mlo_txwi_trace *v),
	TP_ARGS(dev, v),
	TP_STRUCT__entry(
		DEV_ENTRY
			__field(u16, token) __field(u16, pid) __field(u16, wcid)
			__field(u8, link) __field(u8, sel_phy)
			__field(s16, mconf_link) __field(s16, bss)
			__field(s16, omac) __field(s16, wmm)
			__field(u16, wlan) __field(u8, txd_qidx) __field(u8, tgid)
			__field(u16, l3_proto) __field(u8, l4_proto)
			__field(u16, arp_op) __field(u8, icmp_type)
			__field(u8, icmp_code) __field(u16, icmp_id)
			__field(u16, icmp_seq)
			__field(s16, key_idx) __field(u32, key_cipher)
			__field(s16, hw_key_idx) __field(u16, wcid_cipher)
			__field(bool, protect) __field(u8, tid)
		__field(bool, ba_disable) __field(bool, hw_amsdu)
		__field(bool, sn_valid) __field(u16, seq)
		__field(u8, qid) __field(u8, dma_phy) __field(s16, ring)
	),
	TP_fast_assign(
		DEVICE_ASSIGN;
		__entry->token = v->token; __entry->pid = v->pid;
		__entry->wcid = v->wcid; __entry->link = v->link;
		__entry->sel_phy = v->sel_phy; __entry->mconf_link = v->mconf_link;
			__entry->bss = v->bss; __entry->omac = v->omac; __entry->wmm = v->wmm;
			__entry->wlan = v->wlan; __entry->txd_qidx = v->txd_qidx;
			__entry->tgid = v->tgid; __entry->key_idx = v->key_idx;
			__entry->l3_proto = v->l3_proto; __entry->l4_proto = v->l4_proto;
			__entry->arp_op = v->arp_op; __entry->icmp_type = v->icmp_type;
			__entry->icmp_code = v->icmp_code; __entry->icmp_id = v->icmp_id;
			__entry->icmp_seq = v->icmp_seq;
			__entry->key_cipher = v->key_cipher; __entry->hw_key_idx = v->hw_key_idx;
			__entry->wcid_cipher = v->wcid_cipher; __entry->protect = v->protect;
			__entry->tid = v->tid; __entry->ba_disable = v->ba_disable;
		__entry->hw_amsdu = v->hw_amsdu; __entry->sn_valid = v->sn_valid;
		__entry->seq = v->seq; __entry->qid = v->qid;
		__entry->dma_phy = v->dma_phy; __entry->ring = v->ring;
	),
		TP_printk(DEV_PR_FMT " token=%u pid=%u sel=%u/%u/%u mconf=%d bss=%d omac=%d wmm=%d wlan=%u qidx=%u tgid=%u l3=0x%04x l4p=%u arp_op=%u icmp=%u/%u id=%u seq=%u key=%d/0x%x hwkey=%d cipher=%u protect=%u tid=%u ba_dis=%u amsdu=%u sn=%u/%u qid=%u dma_phy=%u ring=%d",
			DEV_PR_ARG, __entry->token, __entry->pid, __entry->wcid,
			__entry->link, __entry->sel_phy, __entry->mconf_link,
			__entry->bss, __entry->omac, __entry->wmm, __entry->wlan,
			__entry->txd_qidx, __entry->tgid,
			__entry->l3_proto, __entry->l4_proto, __entry->arp_op,
			__entry->icmp_type, __entry->icmp_code, __entry->icmp_id,
			__entry->icmp_seq, __entry->key_idx,
			__entry->key_cipher, __entry->hw_key_idx,
			__entry->wcid_cipher, __entry->protect, __entry->tid,
			__entry->ba_disable, __entry->hw_amsdu, __entry->sn_valid,
		__entry->seq, __entry->qid, __entry->dma_phy, __entry->ring)
);

TRACE_EVENT(mlo_txfree,
	TP_PROTO(struct mt76_dev *dev, u16 wcid, s16 link, u16 token,
		 u8 attempts, u8 status, bool failed),
	TP_ARGS(dev, wcid, link, token, attempts, status, failed),
	TP_STRUCT__entry(
		DEV_ENTRY
		__field(u16, wcid) __field(s16, link) __field(u16, token)
		__field(u8, attempts) __field(u8, status) __field(bool, failed)
	),
	TP_fast_assign(
		DEVICE_ASSIGN; __entry->wcid = wcid; __entry->link = link;
		__entry->token = token; __entry->attempts = attempts;
		__entry->status = status; __entry->failed = failed;
	),
	TP_printk(DEV_PR_FMT " wcid=%u link=%d token=%u attempts=%u status=%u failed=%u",
		DEV_PR_ARG, __entry->wcid, __entry->link, __entry->token,
		__entry->attempts, __entry->status, __entry->failed)
);

TRACE_EVENT(mlo_txs,
	TP_PROTO(struct mt76_dev *dev, u16 wcid, s16 link, u8 pid,
		 u8 ack_error, bool acked, u16 tx_rate, u8 bw, u8 txs_band,
		 u8 phy_idx),
	TP_ARGS(dev, wcid, link, pid, ack_error, acked, tx_rate, bw, txs_band,
		phy_idx),
	TP_STRUCT__entry(
		DEV_ENTRY
		__field(u16, wcid) __field(s16, link) __field(u8, pid)
		__field(u8, ack_error) __field(bool, acked)
		__field(u16, tx_rate) __field(u8, bw)
		__field(u8, txs_band) __field(u8, phy_idx)
	),
	TP_fast_assign(
		DEVICE_ASSIGN; __entry->wcid = wcid; __entry->link = link;
		__entry->pid = pid; __entry->ack_error = ack_error;
		__entry->acked = acked; __entry->tx_rate = tx_rate;
		__entry->bw = bw; __entry->txs_band = txs_band;
		__entry->phy_idx = phy_idx;
	),
	TP_printk(DEV_PR_FMT " wcid=%u link=%d pid=%u ack_error=%u acked=%u rate=0x%x bw=%u txs_band=%u wcid_phy_idx=%u",
		DEV_PR_ARG, __entry->wcid, __entry->link, __entry->pid,
		__entry->ack_error, __entry->acked, __entry->tx_rate,
		__entry->bw, __entry->txs_band, __entry->phy_idx)
);

	TRACE_EVENT(mlo_rx,
		TP_PROTO(struct mt76_dev *dev, s16 wcid, s16 link, u8 band,
			 u16 channel, const u8 *sa, const u8 *da,
			 const struct mt76_mlo_pkt_sig *sig, u8 sec_mode, u8 key_id,
			 bool decrypted, u32 drop_flags),
			TP_ARGS(dev, wcid, link, band, channel, sa, da, sig,
				 sec_mode, key_id, decrypted, drop_flags),
		TP_STRUCT__entry(
			DEV_ENTRY
			__field(s16, wcid) __field(s16, link) __field(u8, band)
			__field(u16, channel) __array(u8, sa, ETH_ALEN)
			__array(u8, da, ETH_ALEN) __field(u16, protocol)
			__field(u16, l3_proto) __field(u8, l4_proto)
			__field(u16, arp_op) __field(u8, icmp_type)
			__field(u8, icmp_code) __field(u16, icmp_id)
			__field(u16, icmp_seq)
			__field(u8, sec_mode) __field(u8, key_id)
			__field(bool, decrypted) __field(u32, drop_flags)
		),
	TP_fast_assign(
		DEVICE_ASSIGN; __entry->wcid = wcid; __entry->link = link;
			__entry->band = band; __entry->channel = channel;
			memcpy(__entry->sa, sa, ETH_ALEN); memcpy(__entry->da, da, ETH_ALEN);
				__entry->protocol = sig->l3_proto; __entry->sec_mode = sec_mode;
			__entry->l3_proto = sig->l3_proto; __entry->l4_proto = sig->l4_proto;
			__entry->arp_op = sig->arp_op; __entry->icmp_type = sig->icmp_type;
			__entry->icmp_code = sig->icmp_code; __entry->icmp_id = sig->icmp_id;
			__entry->icmp_seq = sig->icmp_seq;
			__entry->key_id = key_id; __entry->decrypted = decrypted;
			__entry->drop_flags = drop_flags;
		),
		TP_printk(DEV_PR_FMT " wcid=%d link=%d band=%u chan=%u sa=%pM da=%pM proto=0x%04x l3=0x%04x l4p=%u arp_op=%u icmp=%u/%u id=%u seq=%u sec=%u key=%u decrypted=%u flags=0x%x",
			DEV_PR_ARG, __entry->wcid, __entry->link, __entry->band,
			__entry->channel, __entry->sa, __entry->da, __entry->protocol,
			__entry->l3_proto, __entry->l4_proto, __entry->arp_op,
			__entry->icmp_type, __entry->icmp_code, __entry->icmp_id,
			__entry->icmp_seq,
			__entry->sec_mode, __entry->key_id, __entry->decrypted,
			__entry->drop_flags)
	);

#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

#include <trace/define_trace.h>
