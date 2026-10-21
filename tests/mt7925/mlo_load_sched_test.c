// SPDX-License-Identifier: BSD-3-Clause-Clear
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define BIT(n) (1U << (n))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define div64_u64(a, b) ((u64)(a) / (u64)(b))

#include "../../mt792x_mlo_sched.h"

static int expect_u32(const char *name, u32 want, u32 got)
{
	if (want == got)
		return 0;

	fprintf(stderr, "%s: expected %u, got %u\n", name, want, got);
	return 1;
}

int main(void)
{
	u32 spare[3] = { 320, 0, 20 };
	u32 weight[3] = {};
	int failures = 0;

	failures += expect_u32("ordinary delta", 25000,
		mt7925_mlo_airtime_delta(125000, 100000));
	failures += expect_u32("counter reset delta", 7000,
		mt7925_mlo_airtime_delta(7000, BIT(30) + 9000));
	failures += expect_u32("raw utilization uses elapsed time", 50,
		mt7925_mlo_raw_util(125000, 250000));
	failures += expect_u32("raw utilization clamps", 100,
		mt7925_mlo_raw_util(300000, 250000));
	failures += expect_u32("ema rises by alpha one quarter", 25,
		mt7925_mlo_ema_util(0, 100));
	failures += expect_u32("ema decays on zero delta", 18,
		mt7925_mlo_ema_util(25, 0));

	mt7925_mlo_normalize_weights(spare, weight, 3, 1024);
	failures += expect_u32("idle wide normalized weight", 963, weight[0]);
	failures += expect_u32("idle narrow normalized weight", 61, weight[2]);

	spare[0] = 80;
	spare[2] = 20;
	mt7925_mlo_normalize_weights(spare, weight, 3, 1024);
	failures += expect_u32("loaded wide normalized weight", 819, weight[0]);
	failures += expect_u32("idle narrow gains share", 205, weight[2]);

	spare[0] = 320;
	spare[2] = 5;
	mt7925_mlo_normalize_weights(spare, weight, 3, 1024);
	failures += expect_u32("loaded narrow loses share", 16, weight[2]);

	failures += expect_u32("active cache survives 30 second gap", 1,
		mt7925_mlo_cache_alive(1000, 31000));
	failures += expect_u32("inactive cache eventually expires", 0,
		mt7925_mlo_cache_alive(1000, 122000));

	return failures ? 1 : 0;
}
