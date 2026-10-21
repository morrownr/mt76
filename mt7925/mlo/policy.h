/* SPDX-License-Identifier: BSD-3-Clause-Clear */

#ifndef __MT7925_MLO_POLICY_H
#define __MT7925_MLO_POLICY_H

static inline bool
mt7925_mlo_device_supported(bool mt7927, bool mlo_capable)
{
	return mt7927 && mlo_capable;
}

static inline bool
mt7925_mlo_request_eligible(bool mt7927, bool mlo_capable,
			    bool managed, bool mld)
{
	return mt7925_mlo_device_supported(mt7927, mlo_capable) &&
	       managed && mld;
}

static inline bool
mt7925_mlo_ttlm_mapping_valid(u16 valid_links,
			      const u16 *downlink, const u16 *uplink)
{
	u16 links;
	u16 map = downlink[0];
	int n_links = 0, tid;

	if (!map || (map & ~valid_links))
		return false;

	for (links = map; links; links &= links - 1)
		n_links++;
	if (n_links > 2)
		return false;

	for (tid = 0; tid < IEEE80211_TTLM_NUM_TIDS; tid++)
		if (downlink[tid] != map || uplink[tid] != map)
			return false;

	return true;
}

#endif
