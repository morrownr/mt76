/* SPDX-License-Identifier: BSD-3-Clause-Clear */

#ifndef __MT7925_MLO_H
#define __MT7925_MLO_H

#include "../mt7925.h"

void mt7925_mlo_update_caps(struct mt792x_phy *phy,
			    u16 *mld_capa_and_ops);
int mt7925_mlo_ttlm_request(struct ieee80211_vif *vif, u16 links);
void mt7925_mlo_fw_cap_parse(struct mt792x_dev *dev, const u8 *data, int len);
enum ieee80211_neg_ttlm_res
mt7925_mlo_can_neg_ttlm(struct ieee80211_hw *hw,
			struct ieee80211_vif *vif,
			struct ieee80211_neg_ttlm *ttlm);

#endif
