/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT792X_MLO_SCHED_H
#define __MT792X_MLO_SCHED_H

#define MT7925_MLO_UTIL_FLOOR 25
#define MT7925_MLO_FC_AGE_MS 120000

static inline u64 mt7925_mlo_airtime_delta(u64 cur, u64 prev)
{
	/* WTBL admission counters are explicitly cleared, not allowed to
	 * wrap naturally. After a backwards step, cur is therefore all of
	 * the airtime observed since that clear.
	 */
	return cur >= prev ? cur - prev : cur;
}

static inline u32 mt7925_mlo_raw_util(u64 delta_us, u64 elapsed_us)
{
	if (!elapsed_us)
		return 0;

	return min_t(u32, 100,
		     (u32)div64_u64(delta_us * 100, elapsed_us));
}

static inline u8 mt7925_mlo_ema_util(u8 old_util, u32 raw_util)
{
	return (u8)((old_util * 3 + min_t(u32, raw_util, 100)) / 4);
}

static inline u32 mt7925_mlo_spare_weight(u32 base, u8 util)
{
	u32 avail = 100 - min_t(u8, util, 100);

	avail = max_t(u32, avail, MT7925_MLO_UTIL_FLOOR);
	return max_t(u32, 1, (u32)div64_u64((u64)base * avail, 100));
}

static inline void
mt7925_mlo_normalize_weights(const u32 *spare, u32 *weight, int n,
			     u32 buckets)
{
	u64 total = 0;
	u32 assigned = 0;
	int i, last = -1;

	for (i = 0; i < n; i++) {
		weight[i] = 0;
		if (!spare[i])
			continue;
		total += spare[i];
		last = i;
	}

	if (!total)
		return;

	for (i = 0; i < n; i++) {
		if (!spare[i])
			continue;
		if (i == last)
			weight[i] = buckets - assigned;
		else
			weight[i] = (u32)div64_u64((u64)spare[i] * buckets,
						      total);
		assigned += weight[i];
	}
}

static inline bool mt7925_mlo_cache_alive(u64 last_seen_ms, u64 now_ms)
{
	return now_ms - last_seen_ms < MT7925_MLO_FC_AGE_MS;
}

#endif
