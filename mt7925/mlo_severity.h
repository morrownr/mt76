/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7925_MLO_SEVERITY_H
#define __MT7925_MLO_SEVERITY_H

#include "mlo_pair_transition.h"

/* Pure severity memory and last-good MLD record for the MT7927 steering policy.
 *
 * A link set that needed a firmware reset is marked SEVERE so Policy V2 keeps
 * to the remaining options instead of walking into the same failure again.
 * Severity is keyed by BAND class, never by link id: after a reset the AP may
 * hand back a different link set and numbering, so the mark has to describe
 * "2.4+5 GHz" or "single 6 GHz", not "bitmap 0x3". The last-good record keeps the
 * band map of the first genuine three-band 0x7 (a few dozen bytes) so the
 * previous connection can be classified in its own numbering after the reset.
 * No kernel headers: exercised by a userspace test.
 */

/* Graded avoidance: the first strike against a class is short, every repeat
 * doubles it, never beyond the former flat time. The strikes are forgotten after
 * a quiet hour, so one old failure does not lengthen a new one.
 */
#define MT7925_MLO_SEV_BASE_MS	120000ULL	/* first strike */
#define MT7925_MLO_SEV_TTL_MS	1800000ULL	/* longest avoidance */
#define MT7925_MLO_SEV_FORGET_MS 3600000ULL	/* quiet time that forgets the strikes */

enum mt7925_mlo_sev_class {
	MT7925_MLO_SEV_PAIR_24_5,
	MT7925_MLO_SEV_PAIR_24_6,
	MT7925_MLO_SEV_SINGLE_5,
	MT7925_MLO_SEV_SINGLE_6,
	MT7925_MLO_SEV_NUM,
};

struct mt7925_mlo_sev {
	unsigned long long until_ms[MT7925_MLO_SEV_NUM];
	unsigned long long last_ms[MT7925_MLO_SEV_NUM];
	unsigned int marks[MT7925_MLO_SEV_NUM];
	unsigned int strikes[MT7925_MLO_SEV_NUM];
};

struct mt7925_mlo_lastgood {
	int valid;
	unsigned int valid_links;
	int band_of_link[3];
	unsigned long long epoch;
};

/* Band class of a link set in the given numbering, or -1 when the set is not a
 * class: the 5+6 GHz bitmap, three links, a lone 2.4 GHz link, or any bit
 * beyond link 2.
 */
static inline int
mt7925_mlo_sev_class_of(unsigned int links, const int band_of_link[3])
{
	unsigned int bands = 0;
	int n = 0, l;

	if (!links || (links & ~7u))
		return -1;
	for (l = 0; l < 3; l++) {
		if (!(links & (1u << l)))
			continue;
		bands |= 1u << band_of_link[l];
		n++;
	}
	if (n == 2) {
		if (bands == ((1u << MT7925_MLO_BAND_2G) | (1u << MT7925_MLO_BAND_5G)))
			return MT7925_MLO_SEV_PAIR_24_5;
		if (bands == ((1u << MT7925_MLO_BAND_2G) | (1u << MT7925_MLO_BAND_6G)))
			return MT7925_MLO_SEV_PAIR_24_6;
		return -1;
	}
	if (n == 1) {
		if (bands == (1u << MT7925_MLO_BAND_5G))
			return MT7925_MLO_SEV_SINGLE_5;
		if (bands == (1u << MT7925_MLO_BAND_6G))
			return MT7925_MLO_SEV_SINGLE_6;
	}
	return -1;
}

/* Mark a class severe; returns how long it is avoided (0 for no class). */
static inline unsigned long long
mt7925_mlo_sev_mark(struct mt7925_mlo_sev *s, int cls, unsigned long long now_ms)
{
	unsigned long long ttl;

	if (cls < 0 || cls >= MT7925_MLO_SEV_NUM)
		return 0;
	if (s->strikes[cls] && now_ms - s->last_ms[cls] > MT7925_MLO_SEV_FORGET_MS)
		s->strikes[cls] = 0;
	if (s->strikes[cls] < 16)
		s->strikes[cls]++;
	ttl = s->strikes[cls] > 5 ? MT7925_MLO_SEV_TTL_MS :
	      MT7925_MLO_SEV_BASE_MS << (s->strikes[cls] - 1);
	if (ttl > MT7925_MLO_SEV_TTL_MS)
		ttl = MT7925_MLO_SEV_TTL_MS;
	s->until_ms[cls] = now_ms + ttl;
	s->last_ms[cls] = now_ms;
	s->marks[cls]++;
	return ttl;
}

/* A transition that completed (the pair the second caller decided is the pair now
 * running) is not a repeat failure of anything: forget the strikes so the next
 * switch starts from the base time again. The running avoidance stays as
 * hysteresis against flip-flopping. Strikes keep escalating only when the decided
 * pair did not come up.
 */
static inline void
mt7925_mlo_sev_settle(struct mt7925_mlo_sev *s)
{
	int cls;

	for (cls = 0; cls < MT7925_MLO_SEV_NUM; cls++)
		s->strikes[cls] = 0;
}

static inline int
mt7925_mlo_sev_active(const struct mt7925_mlo_sev *s, int cls, unsigned long long now_ms)
{
	if (cls < 0 || cls >= MT7925_MLO_SEV_NUM)
		return 0;
	return now_ms < s->until_ms[cls];
}

/* Keep the band map of the first genuine three-band 0x7. A smaller or partial
 * association never replaces it. Returns 1 when the record was (re)captured.
 */
static inline int
mt7925_mlo_lastgood_capture(struct mt7925_mlo_lastgood *g, unsigned int valid_links,
			    const int band_of_link[3], unsigned int band_present,
			    unsigned long long epoch)
{
	int l;

	if (valid_links != 0x7 || band_present != 0x7)
		return 0;
	g->valid = 1;
	g->valid_links = valid_links;
	for (l = 0; l < 3; l++)
		g->band_of_link[l] = band_of_link[l];
	g->epoch = epoch;
	return 1;
}

static inline int
mt7925_mlo_lastgood_class_of(const struct mt7925_mlo_lastgood *g, unsigned int links)
{
	if (!g->valid)
		return -1;
	return mt7925_mlo_sev_class_of(links, g->band_of_link);
}

/* Link mask of a class in the given numbering, limited to usable links; 0 when a
 * band the class needs has no usable link.
 */
static inline unsigned int
mt7925_mlo_sev_class_links(int cls, const int band_of_link[3], unsigned int usable)
{
	static const int need[MT7925_MLO_SEV_NUM][2] = {
		[MT7925_MLO_SEV_PAIR_24_5] = { MT7925_MLO_BAND_2G, MT7925_MLO_BAND_5G },
		[MT7925_MLO_SEV_PAIR_24_6] = { MT7925_MLO_BAND_2G, MT7925_MLO_BAND_6G },
		[MT7925_MLO_SEV_SINGLE_5] = { MT7925_MLO_BAND_5G, -1 },
		[MT7925_MLO_SEV_SINGLE_6] = { MT7925_MLO_BAND_6G, -1 },
	};
	unsigned int mask = 0;
	int i, l;

	if (cls < 0 || cls >= MT7925_MLO_SEV_NUM)
		return 0;
	for (i = 0; i < 2 && need[cls][i] >= 0; i++) {
		for (l = 0; l < 3; l++)
			if ((usable & (1u << l)) && band_of_link[l] == need[cls][i])
				break;
		if (l == 3)
			return 0;
		mask |= 1u << l;
	}
	return mask;
}

/* The second caller (after the reset) decides what to run. The first caller marked
 * the PREVIOUS connection severe, so the choice is the first class that is not
 * severe and whose bands are usable: pair 2.4+5, pair 2.4+6, then a single
 * high-band link (non-EMLSR), 5 GHz first. Returns 0 when nothing is severe
 * (no override of the automatic choice) or when no option is left. The 5+6 GHz
 * bitmap is never a class, so it is never chosen.
 */
static inline unsigned int
mt7925_mlo_sev_decide(const struct mt7925_mlo_sev *s, const int band_of_link[3],
		      unsigned int usable, unsigned long long now_ms)
{
	int cls, any = 0;

	for (cls = 0; cls < MT7925_MLO_SEV_NUM; cls++)
		if (mt7925_mlo_sev_active(s, cls, now_ms))
			any = 1;
	if (!any)
		return 0;
	for (cls = 0; cls < MT7925_MLO_SEV_NUM; cls++) {
		unsigned int m;

		if (mt7925_mlo_sev_active(s, cls, now_ms))
			continue;
		m = mt7925_mlo_sev_class_links(cls, band_of_link, usable);
		if (m)
			return m;
	}
	return 0;
}

#endif
