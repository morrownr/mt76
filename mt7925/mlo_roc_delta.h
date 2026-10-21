/* SPDX-License-Identifier: ISC */
#ifndef __MT7925_MLO_ROC_DELTA_H
#define __MT7925_MLO_ROC_DELTA_H

/* Pure pair-delta helpers for the MLO JOIN ROC reservation. An MLO JOIN
 * token owns one independently abortable firmware slot per link. On a
 * pair substitution only the slot of the removed link has to disappear;
 * the survivor's slot backs its live channel/security context and
 * aborting it (the old release-everything behaviour) turns the survivor
 * into cm=1/dec=0 RX. Association setup/teardown (either side empty) and
 * malformed masks keep the release-everything semantics.
 */

#ifdef __KERNEL__
#define MT7925_MLO_ROC_HWEIGHT(x)	hweight16(x)
#else
#define MT7925_MLO_ROC_HWEIGHT(x)	__builtin_popcount(x)
#endif

struct mt7925_mlo_roc_delta {
	unsigned int removed;
	unsigned int survivor;
	unsigned int added;
	unsigned int legal_swap;
};

static inline void
mt7925_mlo_roc_delta(unsigned int old_links, unsigned int new_links,
		     unsigned int valid_links, struct mt7925_mlo_roc_delta *d)
{
	d->removed = old_links & ~new_links;
	d->survivor = old_links & new_links;
	d->added = new_links & ~old_links;
	d->legal_swap = old_links != new_links &&
			MT7925_MLO_ROC_HWEIGHT(old_links) == 2 &&
			MT7925_MLO_ROC_HWEIGHT(new_links) == 2 &&
			((old_links | new_links) & ~valid_links) == 0 &&
			MT7925_MLO_ROC_HWEIGHT(d->removed) == 1 &&
			MT7925_MLO_ROC_HWEIGHT(d->survivor) == 1 &&
			MT7925_MLO_ROC_HWEIGHT(d->added) == 1;
}

/* Slots of the old token (token_links) to abort for old_links->new_links.
 * Shrink-to-survivor and full swap both release only the removed links.
 * Anything that is not a plain link removal keeps release-everything.
 */
static inline unsigned int
mt7925_mlo_roc_release_mask(unsigned int old_links, unsigned int new_links,
			    unsigned int valid_links, unsigned int token_links)
{
	if (old_links == new_links)
		return 0;
	if (!old_links || !new_links ||
	    ((old_links | new_links) & ~valid_links) ||
	    MT7925_MLO_ROC_HWEIGHT(new_links) > 2)
		return token_links;
	return token_links & old_links & ~new_links;
}

/* Replacement JOIN for sel_links: keep slots of links that stay selected,
 * free every other slot the previous token still holds.
 */
static inline unsigned int
mt7925_mlo_roc_stale_release_mask(unsigned int token_links,
				  unsigned int sel_links)
{
	return token_links & ~sel_links;
}

#endif
