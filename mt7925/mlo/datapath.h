/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7925_MLO_DATAPATH_H
#define __MT7925_MLO_DATAPATH_H
#include "../mt7925.h"

/* RX path retains RCU protection and ownership of generic parsing/decryption. */
void mt7925_mlo_rx_route(struct mt792x_dev *dev,
			 struct mt76_rx_status *status, bool unicast,
			 u16 fw_idx, u8 desc_band_idx);
void mt7925_mlo_rx_account(struct sk_buff *skb, bool unicast, __le16 fc,
			   u32 rxd1, u32 rxd2, u32 rxd3);
struct mt7925_mlo_rx_diag {
	struct mt76_mlo_pkt_sig sig;
	__be32 src_ip, dst_ip;
	u32 tcp_seq, tcp_ack;
	u16 sport, dport, ip_id, fc, seq_ctrl;
	u8 sa[ETH_ALEN], da[ETH_ALEN], ra[ETH_ALEN], ta[ETH_ALEN], tid;
	bool mac_valid, ip_valid, l4_valid, truncated;
};

void mt7925_mlo_rx_diag_parse(struct sk_buff *skb, u32 flags, u8 sec_mode,
			     bool amsdu, struct mt7925_mlo_rx_diag *d);
void mt7925_mlo_raw_completion(struct mt792x_dev *dev, enum rx_pkt_type type,
			       const void *data, int len);
#endif
