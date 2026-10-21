/* SPDX-License-Identifier: BSD-3-Clause-Clear */

#ifndef __MT7925_MLO_INTERNAL_H
#define __MT7925_MLO_INTERNAL_H

#include "mlo.h"

enum mt7925_mlo_action {
	MT7925_MLO_ACTION_UPDATE_CAPS,
	MT7925_MLO_ACTION_NEG_TTLM,
};

struct mt7925_mlo_request {
	enum mt7925_mlo_action action;
	union {
		u16 *mld_capa_and_ops;
		struct {
			struct ieee80211_vif *vif;
			struct ieee80211_neg_ttlm *ttlm;
		} neg_ttlm;
	};
};

void mt7925_mlo_ttlm_update_caps(u16 *mld_capa_and_ops);
bool mt7925_mlo_ttlm_validate(struct ieee80211_vif *vif,
			      const struct ieee80211_neg_ttlm *ttlm);

#endif
