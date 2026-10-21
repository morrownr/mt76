// SPDX-License-Identifier: BSD-3-Clause-Clear
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef uint16_t u16;

#define IEEE80211_TTLM_NUM_TIDS 8

#include "../../mt7925/mlo/policy.h"

static int expect_eligibility(const char *name, bool expected, bool mt7927,
			      bool mlo_capable, bool managed, bool mld)
{
	bool actual = mt7925_mlo_request_eligible(mt7927, mlo_capable,
						 managed, mld);

	if (actual == expected)
		return 0;

	fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
	return 1;
}

static void fill_maps(u16 *downlink, u16 *uplink, u16 map)
{
	int tid;

	for (tid = 0; tid < IEEE80211_TTLM_NUM_TIDS; tid++) {
		downlink[tid] = map;
		uplink[tid] = map;
	}
}

static int expect_mapping(const char *name, bool expected, u16 valid_links,
			  u16 active_links,
			  const u16 *downlink,
			  const u16 *uplink)
{
	bool actual = mt7925_mlo_ttlm_mapping_valid(valid_links, active_links,
						     downlink, uplink);

	if (actual == expected)
		return 0;

	fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
	return 1;
}

int main(void)
{
	u16 downlink[IEEE80211_TTLM_NUM_TIDS];
	u16 uplink[IEEE80211_TTLM_NUM_TIDS];
	int failures = 0;

	failures += expect_eligibility("MT7927 managed MLD", true, true, true,
					true, true);
	failures += expect_eligibility("monitor interface", false, true, true,
					false, true);
	failures += expect_eligibility("managed non-MLO", false, true, true,
					true, false);
	failures += expect_eligibility("MLO capability disabled", false, true,
					false, true, true);
	failures += expect_eligibility("MT7925 remains unsupported", false,
					false, true, true, true);

	fill_maps(downlink, uplink, 0x0001);
	failures += expect_mapping("single-link uniform map", true,
				   0x0003, 0x0003, downlink, uplink);

	fill_maps(downlink, uplink, 0x0003);
	failures += expect_mapping("two-link uniform map", true,
				   0x0003, 0x0003, downlink, uplink);
	failures += expect_mapping("two-link map over three valid links", true,
				   0x0007, 0x0003, downlink, uplink);

	fill_maps(downlink, uplink, 0x0007);
	failures += expect_mapping("three-link map", false, 0x0007, 0x0007,
				   downlink, uplink);

	fill_maps(downlink, uplink, 0x0000);
	failures += expect_mapping("empty map", false, 0x0003, 0x0003,
				   downlink, uplink);

	fill_maps(downlink, uplink, 0x0004);
	failures += expect_mapping("map outside valid links", false,
				   0x0003, 0x0007,
				   downlink, uplink);
	failures += expect_mapping("inactive valid link", false,
				   0x0007, 0x0003,
				   downlink, uplink);

	fill_maps(downlink, uplink, 0x0005);
	failures += expect_mapping("partially inactive map", false,
				   0x0007, 0x0003,
				   downlink, uplink);

	fill_maps(downlink, uplink, 0x0005);
	failures += expect_mapping("partially invalid map", false,
				   0x0003, 0x0007,
				   downlink, uplink);

	fill_maps(downlink, uplink, 0x0001);
	uplink[0] = 0x0002;
	failures += expect_mapping("TID 0 direction mismatch", false,
				   0x0003, 0x0003, downlink, uplink);

	fill_maps(downlink, uplink, 0x0001);
	uplink[3] = 0x0002;
	failures += expect_mapping("direction mismatch", false,
				   0x0003, 0x0003,
				   downlink, uplink);

	fill_maps(downlink, uplink, 0x0001);
	downlink[7] = 0x0002;
	uplink[7] = 0x0002;
	failures += expect_mapping("per-TID map mismatch", false,
				   0x0003, 0x0003,
				   downlink, uplink);

	if (failures)
		return 1;

	puts("all mt7925 TTLM mapping tests passed");
	return 0;
}
