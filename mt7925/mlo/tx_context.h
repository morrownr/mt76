/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7927_MLO_TX_CONTEXT_H
#define __MT7927_MLO_TX_CONTEXT_H

struct mt7925e_mlo_tx_context {
	struct mt76_wcid *owner;
	u8 omac, wmm, band;
	u8 ra[ETH_ALEN], ta[ETH_ALEN];
};
int mt7925e_mlo_tx_pn_context(struct mt76_dev *mdev, struct ieee80211_sta *sta,
	struct sk_buff *skb, struct mt76_wcid *live, int qid,
	struct mt7925e_mlo_tx_context *ctx);
#endif
