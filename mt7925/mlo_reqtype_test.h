/* SPDX-License-Identifier: ISC */
#ifndef __MT7925_MLO_REQTYPE_TEST_H
#define __MT7925_MLO_REQTYPE_TEST_H

/* Wire reqtype values, not ROC TLV tags. AUTO preserves existing policy. */
static inline int mt7925_mlo_test_reqtype_valid(int override)
{
	return override == -1 || override == 0 ||
	       override == 10 || override == 11;
}

static inline int
mt7925_mlo_test_reqtype_resolve(int override, unsigned int is_delta,
			      int auto_type)
{
	if (is_delta && override >= 0 &&
	    mt7925_mlo_test_reqtype_valid(override))
		return override;
	return auto_type;
}

#endif
