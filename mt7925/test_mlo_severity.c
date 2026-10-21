/* Standalone contract test for the MT7927 severity memory and last-good MLD record.
 *   gcc -Wall -Wextra -Werror -I mt7925 -o /tmp/t test_mlo_severity.c && /tmp/t
 *
 * Severity is keyed by BAND class, never by link id: after a firmware reset the
 * AP may hand back a different link set and numbering, so "the pair that was
 * lost" must survive a renumbering. Classes: 2.4+5, 2.4+6, single 5, single 6.
 * The 5+6 bitmap, three links and a lone 2.4 GHz link are not classes.
 */
#include <stdio.h>
#include <string.h>
#define BIT_(n) (1u << (n))
#include "mlo_severity.h"

static int failures;

static void check(const char *name, long got, long want)
{
	if (got != want) {
		printf("FAIL %s: got=%ld want=%ld\n", name, got, want);
		failures++;
	} else {
		printf("PASS %s\n", name);
	}
}

int main(void)
{
	/* project numbering: L0=2.4, L1=5, L2=6 */
	const int usual[3] = { MT7925_MLO_BAND_2G, MT7925_MLO_BAND_5G, MT7925_MLO_BAND_6G };
	/* a different association: L0=5, L1=2.4, L2=6 */
	const int swapped[3] = { MT7925_MLO_BAND_5G, MT7925_MLO_BAND_2G, MT7925_MLO_BAND_6G };
	struct mt7925_mlo_sev s;
	struct mt7925_mlo_lastgood g;

	/* A: classification by band */
	check("A_0x3_is_pair_24_5", mt7925_mlo_sev_class_of(0x3, usual), MT7925_MLO_SEV_PAIR_24_5);
	check("A_0x5_is_pair_24_6", mt7925_mlo_sev_class_of(0x5, usual), MT7925_MLO_SEV_PAIR_24_6);
	check("A_0x6_5_plus_6_is_no_class", mt7925_mlo_sev_class_of(0x6, usual), -1);
	check("A_three_links_no_class", mt7925_mlo_sev_class_of(0x7, usual), -1);
	check("A_single_5", mt7925_mlo_sev_class_of(0x2, usual), MT7925_MLO_SEV_SINGLE_5);
	check("A_single_6", mt7925_mlo_sev_class_of(0x4, usual), MT7925_MLO_SEV_SINGLE_6);
	check("A_single_24_is_no_class", mt7925_mlo_sev_class_of(0x1, usual), -1);
	check("A_empty_no_class", mt7925_mlo_sev_class_of(0, usual), -1);
	check("A_bits_above_link2_no_class", mt7925_mlo_sev_class_of(0x9, usual), -1);

	/* B: the same bitmap means a different class when numbering differs */
	check("B_swapped_0x3_is_24_5", mt7925_mlo_sev_class_of(0x3, swapped), MT7925_MLO_SEV_PAIR_24_5);
	check("B_swapped_0x6_is_24_6", mt7925_mlo_sev_class_of(0x6, swapped), MT7925_MLO_SEV_PAIR_24_6);
	check("B_swapped_0x1_is_single_5", mt7925_mlo_sev_class_of(0x1, swapped), MT7925_MLO_SEV_SINGLE_5);

	/* C: mark/active with expiry; other classes stay available */
	memset(&s, 0, sizeof(s));
	check("C_clean_not_active", mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_PAIR_24_5, 1000), 0);
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_5, 1000);
	check("C_marked_active", mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_PAIR_24_5, 1001), 1);
	check("C_other_pair_unaffected", mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_PAIR_24_6, 1001), 0);
	check("C_single_unaffected", mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_SINGLE_5, 1001), 0);
	check("C_active_until_first_strike_end",
	      mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_PAIR_24_5, 1000 + MT7925_MLO_SEV_BASE_MS - 1), 1);
	check("C_expires_at_first_strike_end",
	      mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_PAIR_24_5, 1000 + MT7925_MLO_SEV_BASE_MS), 0);

	/* D: a repeat mark lasts longer than the first and is counted */
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_5, 5000);
	check("D_repeat_is_longer_than_first",
	      mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_PAIR_24_5, 5000 + MT7925_MLO_SEV_BASE_MS + 1), 1);
	check("D_marks_counted", s.marks[MT7925_MLO_SEV_PAIR_24_5], 3 - 1);

	/* E: out-of-range classes are inert */
	mt7925_mlo_sev_mark(&s, -1, 1);
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_NUM, 1);
	check("E_bad_class_never_active", mt7925_mlo_sev_active(&s, -1, 1), 0);
	check("E_bad_class_high_never_active", mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_NUM, 1), 0);

	/* F: last-good record is captured only from a genuine three-band 0x7 */
	memset(&g, 0, sizeof(g));
	check("F_two_links_not_captured",
	      mt7925_mlo_lastgood_capture(&g, 0x3, usual, BIT_(0) | BIT_(1), 11), 0);
	check("F_missing_band_not_captured",
	      mt7925_mlo_lastgood_capture(&g, 0x7, usual, BIT_(0) | BIT_(1), 11), 0);
	check("F_not_valid_after_refusals", g.valid, 0);
	check("F_genuine_0x7_captured",
	      mt7925_mlo_lastgood_capture(&g, 0x7, usual, BIT_(0) | BIT_(1) | BIT_(2), 11), 1);
	check("F_record_valid", g.valid, 1);
	check("F_record_epoch", (long)g.epoch, 11);

	/* G: the record classifies a link set in the numbering of the original 0x7,
	 * so the previous connection is identified even after the AP renumbers */
	check("G_prev_pair_from_record", mt7925_mlo_lastgood_class_of(&g, 0x5), MT7925_MLO_SEV_PAIR_24_6);
	memset(&g, 0, sizeof(g));
	check("G_no_record_no_class", mt7925_mlo_lastgood_class_of(&g, 0x5), -1);

	/* H: a later non-0x7 association must not overwrite the good record */
	mt7925_mlo_lastgood_capture(&g, 0x7, usual, BIT_(0) | BIT_(1) | BIT_(2), 11);
	mt7925_mlo_lastgood_capture(&g, 0x3, swapped, BIT_(0) | BIT_(1), 12);
	check("H_record_kept_epoch", (long)g.epoch, 11);
	check("H_record_kept_map", g.band_of_link[1], MT7925_MLO_BAND_5G);

	/* J: the second caller decides among the remaining options. The first caller
	 * marked the PREVIOUS connection severe (the pair being left), so the choice
	 * is the pair that is not severe. Nothing severe means no override at all. */
	memset(&s, 0, sizeof(s));
	check("J_nothing_severe_no_override", mt7925_mlo_sev_decide(&s, usual, 7, 1000), 0);
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000);	/* was on 0x5, leaving it */
	check("J_prev_2_4_6_severe_chooses_2_4_5", mt7925_mlo_sev_decide(&s, usual, 7, 1001), 0x3);
	memset(&s, 0, sizeof(s));
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_5, 1000);	/* was on 0x3 */
	check("J_prev_2_4_5_severe_chooses_2_4_6", mt7925_mlo_sev_decide(&s, usual, 7, 1001), 0x5);
	check("J_numbering_follows_the_new_association", mt7925_mlo_sev_decide(&s, swapped, 7, 1001), 0x6);

	/* K: a band that is not usable cannot be chosen */
	memset(&s, 0, sizeof(s));
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000);
	check("K_no_5ghz_link_no_pair_choice_falls_to_single_6", mt7925_mlo_sev_decide(&s, usual, 0x5, 1001), 0x4);
	check("K_only_2_4_usable_nothing_to_choose", mt7925_mlo_sev_decide(&s, usual, 0x1, 1001), 0);

	/* L: both pairs severe -> a single high-band link (non-EMLSR), 5 GHz first */
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_5, 1000);
	check("L_both_pairs_severe_single_5", mt7925_mlo_sev_decide(&s, usual, 7, 1001), 0x2);
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_SINGLE_5, 1000);
	check("L_single_5_severe_too_single_6", mt7925_mlo_sev_decide(&s, usual, 7, 1001), 0x4);
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_SINGLE_6, 1000);
	check("L_everything_severe_no_override", mt7925_mlo_sev_decide(&s, usual, 7, 1001), 0);

	/* M: the choice is never the 5+6 GHz bitmap and expiry restores the options */
	memset(&s, 0, sizeof(s));
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_5, 1000);
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000);
	check("M_never_5_plus_6", mt7925_mlo_sev_decide(&s, usual, 7, 1001) == 0x6, 0);
	check("M_expired_marks_no_override", mt7925_mlo_sev_decide(&s, usual, 7, 1000 + MT7925_MLO_SEV_TTL_MS), 0);

	/* N: graded avoidance. The first strike is short, a repeat of the same class
	 * doubles it up to the former flat value, and a quiet hour forgets the strikes.
	 */
	memset(&s, 0, sizeof(s));
	check("N_first_strike_is_the_base_time",
	      (long)mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000), (long)MT7925_MLO_SEV_BASE_MS);
	check("N_first_strike_active_just_before_base",
	      mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_PAIR_24_6, 1000 + MT7925_MLO_SEV_BASE_MS - 1), 1);
	check("N_first_strike_over_at_base",
	      mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_PAIR_24_6, 1000 + MT7925_MLO_SEV_BASE_MS), 0);
	check("N_repeat_doubles",
	      (long)mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000 + 2 * MT7925_MLO_SEV_BASE_MS),
	      (long)(2 * MT7925_MLO_SEV_BASE_MS));
	check("N_third_doubles_again",
	      (long)mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000 + 5 * MT7925_MLO_SEV_BASE_MS),
	      (long)(4 * MT7925_MLO_SEV_BASE_MS));
	check("N_strikes_counted", s.strikes[MT7925_MLO_SEV_PAIR_24_6], 3);
	check("N_other_class_unaffected",
	      (long)mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_5, 2000), (long)MT7925_MLO_SEV_BASE_MS);

	/* O: never longer than the former flat value */
	memset(&s, 0, sizeof(s));
	{
		unsigned long long now = 1000, ttl = 0;
		int i;

		for (i = 0; i < 12; i++) {
			ttl = mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_SINGLE_6, now);
			now += 1000;
		}
		check("O_capped_at_former_flat_time", (long)ttl, (long)MT7925_MLO_SEV_TTL_MS);
	}

	/* P: a quiet hour after the last strike forgets the history */
	memset(&s, 0, sizeof(s));
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000);
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 2000);
	check("P_forgotten_after_a_quiet_hour",
	      (long)mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 2000 + MT7925_MLO_SEV_FORGET_MS + 1),
	      (long)MT7925_MLO_SEV_BASE_MS);
	check("P_strike_count_restarts", s.strikes[MT7925_MLO_SEV_PAIR_24_6], 1);

	/* Q: the A/B figure for the record, flat versus graded avoidance for three
	 * resets of one class, five minutes apart (the recovery rate limit)
	 */
	{
		unsigned long long flat = 0, graded = 0, now = 0, ttl;
		int i;

		memset(&s, 0, sizeof(s));
		for (i = 0; i < 3; i++) {
			ttl = mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, now + 1000);
			graded += ttl;
			flat += MT7925_MLO_SEV_TTL_MS;
			now += 300000;
		}
		printf("INFO Q_three_resets_5min_apart flat_ms=%llu graded_ms=%llu\n", flat, graded);
		check("Q_graded_avoids_less_than_flat", graded < flat, 1);
	}

	/* R: legitimate switching must not lengthen the lockout. A transition that
	 * completed (the decided pair is the one running) settles the strikes; the
	 * running avoidance stays as hysteresis. A transition that did not complete
	 * keeps escalating.
	 */
	memset(&s, 0, sizeof(s));
	{
		unsigned long long now = 1000, ttl = 0;
		int i, same = 1;

		for (i = 0; i < 6; i++) {
			ttl = mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, now);
			if (ttl != MT7925_MLO_SEV_BASE_MS)
				same = 0;
			mt7925_mlo_sev_settle(&s);
			now += 2 * MT7925_MLO_SEV_BASE_MS;
		}
		check("R_six_completed_switches_never_escalate", same, 1);
	}
	memset(&s, 0, sizeof(s));
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000);
	mt7925_mlo_sev_settle(&s);
	check("R_settle_keeps_the_running_avoidance",
	      mt7925_mlo_sev_active(&s, MT7925_MLO_SEV_PAIR_24_6, 1000 + MT7925_MLO_SEV_BASE_MS - 1), 1);
	check("R_settle_forgets_the_strikes", s.strikes[MT7925_MLO_SEV_PAIR_24_6], 0);
	check("R_settle_keeps_the_mark_count", s.marks[MT7925_MLO_SEV_PAIR_24_6], 1);
	memset(&s, 0, sizeof(s));
	mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000);
	check("R_failed_transition_still_escalates",
	      (long)mt7925_mlo_sev_mark(&s, MT7925_MLO_SEV_PAIR_24_6, 1000 + 2 * MT7925_MLO_SEV_BASE_MS),
	      (long)(2 * MT7925_MLO_SEV_BASE_MS));

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures ? 1 : 0;
}
