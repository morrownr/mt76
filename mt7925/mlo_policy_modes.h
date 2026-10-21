/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7925_MLO_POLICY_MODES_H
#define __MT7925_MLO_POLICY_MODES_H

#include "mlo_pair_transition.h"

/* Pure steering-policy mode table for the MT7927.
 *
 * A policy mode ID is a custom operating-mode identifier. It is NOT a link
 * bitmap and NOT a firmware value: 0x4 does not mean "link 2", and mode 0x5/
 * 0x6 do not mean "links 0+2"/"links 1+2". Five different things stay
 * separate and are never derived from one another by guessing:
 *
 *   1. policy mode ID        this table
 *   2. MLO link bitmap       resolved here from the DISCOVERED band->link map
 *   3. link id / band        mac80211 bss_conf channel of each negotiated link
 *   4. physical RF slot      mt7925_mlo_band_slot() (firmware grants: 2.4 GHz
 *                            slot 0, 5 and 6 GHz both slot 1)
 *   5. firmware operating    reported by firmware events, never assumed here
 *      state
 *
 * Simultaneous modes pair 2.4 GHz (slot 0) with ONE high band (slot 1).
 * 5 GHz and 6 GHz share slot 1, so no table entry may transmit on both.
 *
 * EMLSR modes are time-multiplexed and are available only when every
 * prerequisite is established by real evidence. Their link composition is
 * not inferred: it is supplied by the caller (emlsr_links[]) once it is
 * established, and an unestablished composition is reported, never filled
 * in.
 */

enum mt7925_mlo_policy_mode {
	MT7925_MLO_MODE_SIM_24_5 = 0x3,
	MT7925_MLO_MODE_SIM_24_6 = 0x4,
	MT7925_MLO_MODE_EMLSR_5 = 0x5,
	MT7925_MLO_MODE_EMLSR_6 = 0x6,
};

enum mt7925_mlo_mode_avail {
	MT7925_MLO_MODE_OK,
	MT7925_MLO_MODE_UNKNOWN,
	MT7925_MLO_MODE_BAND_ABSENT,
	MT7925_MLO_MODE_AP_NO_EMLSR,
	MT7925_MLO_MODE_LOCAL_NO_EMLSR,
	MT7925_MLO_MODE_FW_NO_EMLSR,
	MT7925_MLO_MODE_NO_SWITCH_PRIMITIVE,
	MT7925_MLO_MODE_COMPOSITION_UNESTABLISHED,
	MT7925_MLO_MODE_NUM_AVAIL,
};

struct mt7925_mlo_mode_ctx {
	/* discovered: link id negotiated on each band, and which bands exist */
	unsigned char band_link[3];
	unsigned int band_present;
	/* capabilities, each from its own real source */
	unsigned int ap_emlsr;			/* AP Multi-Link element EML caps */
	unsigned int local_emlsr_adv;		/* what we advertise to the AP */
	unsigned int fw_emlsr;			/* firmware NIC_CAP EML capability */
	unsigned int switch_primitive_verified;	/* a firmware switch op proven on this chip */
	/* EMLSR link set per mode (index = mode - EMLSR_5), once established */
	unsigned int emlsr_links_known;
	unsigned int emlsr_links[2];
};

struct mt7925_mlo_mode_band_pair {
	int band_a;
	int band_b;
};

static inline int
mt7925_mlo_mode_bands(int mode, struct mt7925_mlo_mode_band_pair *out)
{
	switch (mode) {
	case MT7925_MLO_MODE_SIM_24_5:
		out->band_a = MT7925_MLO_BAND_2G;
		out->band_b = MT7925_MLO_BAND_5G;
		return 1;
	case MT7925_MLO_MODE_SIM_24_6:
		out->band_a = MT7925_MLO_BAND_2G;
		out->band_b = MT7925_MLO_BAND_6G;
		return 1;
	default:
		return 0;
	}
}

static inline int
mt7925_mlo_mode_eval(int mode, const struct mt7925_mlo_mode_ctx *c,
		     unsigned int *links)
{
	struct mt7925_mlo_mode_band_pair bp;

	*links = 0;

	if (mt7925_mlo_mode_bands(mode, &bp)) {
		if (!(c->band_present & (1u << bp.band_a)) ||
		    !(c->band_present & (1u << bp.band_b)))
			return MT7925_MLO_MODE_BAND_ABSENT;
		*links = (1u << c->band_link[bp.band_a]) |
			 (1u << c->band_link[bp.band_b]);
		return MT7925_MLO_MODE_OK;
	}

	if (mode != MT7925_MLO_MODE_EMLSR_5 && mode != MT7925_MLO_MODE_EMLSR_6)
		return MT7925_MLO_MODE_UNKNOWN;

	/* root cause first: the firmware's own non-AP EMLSR statement, then
	 * what we advertise because of it
	 */
	if (!c->ap_emlsr)
		return MT7925_MLO_MODE_AP_NO_EMLSR;
	if (!c->fw_emlsr)
		return MT7925_MLO_MODE_FW_NO_EMLSR;
	if (!c->local_emlsr_adv)
		return MT7925_MLO_MODE_LOCAL_NO_EMLSR;
	if (!c->switch_primitive_verified)
		return MT7925_MLO_MODE_NO_SWITCH_PRIMITIVE;
	if (!c->emlsr_links_known)
		return MT7925_MLO_MODE_COMPOSITION_UNESTABLISHED;

	*links = c->emlsr_links[mode - MT7925_MLO_MODE_EMLSR_5];
	return *links ? MT7925_MLO_MODE_OK : MT7925_MLO_MODE_COMPOSITION_UNESTABLISHED;
}

/* The policy mode the currently published link mask corresponds to, or 0 if
 * it is none of them (e.g. the single-radio 5+6 GHz mask, three links, or a
 * single link): such a state is not a steering mode and must not be scored
 * as though it were one.
 */
static inline int
mt7925_mlo_mode_of_links(unsigned int links, const struct mt7925_mlo_mode_ctx *c)
{
	static const int modes[] = {
		MT7925_MLO_MODE_SIM_24_5, MT7925_MLO_MODE_SIM_24_6,
		MT7925_MLO_MODE_EMLSR_5, MT7925_MLO_MODE_EMLSR_6,
	};
	unsigned int i, m;

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (mt7925_mlo_mode_eval(modes[i], c, &m) == MT7925_MLO_MODE_OK &&
		    m == links)
			return modes[i];
	}
	return 0;
}

/* A simultaneous mode must put its two links on different RF slots. */
static inline int mt7925_mlo_mode_slots_distinct(int mode)
{
	struct mt7925_mlo_mode_band_pair bp;

	if (!mt7925_mlo_mode_bands(mode, &bp))
		return 0;
	return mt7925_mlo_band_slot(bp.band_a) != mt7925_mlo_band_slot(bp.band_b);
}

/* Nonzero if any simultaneous mode would transmit on 5 and 6 GHz at once. */
static inline int mt7925_mlo_mode_simultaneous_high_band(void)
{
	static const int modes[] = {
		MT7925_MLO_MODE_SIM_24_5, MT7925_MLO_MODE_SIM_24_6,
	};
	struct mt7925_mlo_mode_band_pair bp;
	unsigned int i;

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (!mt7925_mlo_mode_bands(modes[i], &bp))
			continue;
		if ((bp.band_a == MT7925_MLO_BAND_5G && bp.band_b == MT7925_MLO_BAND_6G) ||
		    (bp.band_a == MT7925_MLO_BAND_6G && bp.band_b == MT7925_MLO_BAND_5G))
			return 1;
	}
	return 0;
}

static inline const char *mt7925_mlo_mode_reason(int avail)
{
	static const char * const n[] = {
		"ok", "unknown_mode", "band_not_negotiated",
		"ap_no_emlsr", "local_no_emlsr_advertised", "fw_no_emlsr",
		"no_verified_switch_primitive", "emlsr_composition_unestablished",
	};

	return (unsigned int)avail < MT7925_MLO_MODE_NUM_AVAIL ? n[avail] : "?";
}

/* Transition classes. What a transition asks of the firmware, independent of
 * which mode it is named after.
 *
 * REMOVE            only links leave: a shrink, no acquisition (proven live).
 * ADD_LINK          a link is acquired on top of surviving ones.
 * HIGHBAND_RETUNE   the 2.4 GHz link stays and the high band changes (5 <-> 6
 *                   GHz): the one shared radio (slot 1) is released by one
 *                   band and taken by the other, i.e. retuned.
 * REPLACE           anything else that both removes and adds.
 *
 * A class is attempted only after it has completed with authenticated
 * validation in this association, or when an experiment explicitly asks for
 * unverified attempts. Every unverified class failed destructively when it
 * was tried live (Memory 943 and the policy v2 degrade run): the new link
 * never admitted secure RX and the rollback was blocked by the held grant,
 * leaving a single dead link. Verification is earned per association, never
 * assumed.
 */
enum mt7925_mlo_switch_class {
	MT7925_MLO_SWITCH_NONE,
	MT7925_MLO_SWITCH_REMOVE,
	MT7925_MLO_SWITCH_ADD_LINK,
	MT7925_MLO_SWITCH_HIGHBAND_RETUNE,
	MT7925_MLO_SWITCH_REPLACE,
	MT7925_MLO_SWITCH_NUM,
};

static inline unsigned int
mt7925_mlo_switch_class(unsigned int cur, unsigned int tgt, const int band_of_link[3])
{
	unsigned int removed = cur & ~tgt, added = tgt & ~cur, survivor = cur & tgt;

	if (cur == tgt)
		return MT7925_MLO_SWITCH_NONE;
	if (!added)
		return MT7925_MLO_SWITCH_REMOVE;
	if (!removed)
		return MT7925_MLO_SWITCH_ADD_LINK;
	if (MT7925_MLO_PT_HWEIGHT(survivor) == 1 && MT7925_MLO_PT_HWEIGHT(removed) == 1 &&
	    MT7925_MLO_PT_HWEIGHT(added) == 1) {
		int r = band_of_link[MT7925_MLO_PT_FFS(removed)];
		int a = band_of_link[MT7925_MLO_PT_FFS(added)];

		if (r != a && mt7925_mlo_band_slot(r) == mt7925_mlo_band_slot(a))
			return MT7925_MLO_SWITCH_HIGHBAND_RETUNE;
	}
	return MT7925_MLO_SWITCH_REPLACE;
}

static inline int
mt7925_mlo_switch_feasible(unsigned int cls, unsigned int verified_mask,
			   int try_unverified)
{
	if (cls == MT7925_MLO_SWITCH_NONE || cls == MT7925_MLO_SWITCH_REMOVE)
		return 1;
	return try_unverified || (verified_mask & (1u << cls));
}

static inline const char *mt7925_mlo_switch_class_name(unsigned int cls)
{
	static const char * const n[] = {
		"none", "remove", "add_link", "highband_retune", "replace",
	};

	return cls < MT7925_MLO_SWITCH_NUM ? n[cls] : "?";
}

#endif
