// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "internal.h"
#include "policy.h"

void mt7925_mlo_ttlm_update_caps(u16 *mld_capa_and_ops)
{
	u16 support = IEEE80211_MLD_CAP_OP_TID_TO_LINK_MAP_NEG_SUPP_SAME;

	*mld_capa_and_ops |=
		u16_encode_bits(support,
				IEEE80211_MLD_CAP_OP_TID_TO_LINK_MAP_NEG_SUPP);
}

bool mt7925_mlo_ttlm_validate(struct ieee80211_vif *vif,
			      const struct ieee80211_neg_ttlm *ttlm)
{
	return mt7925_mlo_ttlm_mapping_valid(vif->valid_links,
					      ttlm->downlink,
					      ttlm->uplink);
}

/* ieee80211_request_neg_ttlm() exists only in a mac80211 that carries the TTLM
 * request patch; a stock mac80211 does not export it. Referencing it weakly lets
 * the module load either way (the kernel module loader resolves an absent weak
 * symbol to NULL and modpost does not flag it); the request then reports
 * -EOPNOTSUPP. Production steering never reaches this call: it is used only by
 * the test-only `mlo_ttlm_request` debugfs control and by the TTLM-staged
 * debugfs `mlo_active_links` path (test_mt7927_mlo_ttlm_removal, default off).
 */
extern int ieee80211_request_neg_ttlm(struct ieee80211_vif *vif,
				      struct cfg80211_ttlm_params *params) __weak;

/* Request one negotiated TID-to-link mapping that puts every TID, both
 * directions, on @links. The single place that builds this request: the
 * steering controller uses it before a shrink, and the test-only debugfs
 * control uses it on its own. An unusable mapping never reaches mac80211.
 */
int mt7925_mlo_ttlm_request(struct ieee80211_vif *vif, u16 links)
{
	struct cfg80211_ttlm_params params = {};
	u16 map[IEEE80211_TTLM_NUM_TIDS];
	int tid;

	for (tid = 0; tid < IEEE80211_TTLM_NUM_TIDS; tid++) {
		params.dlink[tid] = links;
		params.ulink[tid] = links;
		map[tid] = links;
	}

	if (!mt7925_mlo_ttlm_mapping_valid(vif->valid_links, map, map))
		return -EINVAL;

	if (!ieee80211_request_neg_ttlm)
		return -EOPNOTSUPP;
	return ieee80211_request_neg_ttlm(vif, &params);
}
