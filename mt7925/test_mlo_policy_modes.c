/* Standalone contract test for the MT7927 steering policy mode table.
 *   gcc -Wall -Wextra -Werror -o /tmp/t test_mlo_policy_modes.c && /tmp/t
 *
 * Policy mode IDs are NOT link bitmaps: 0x4 is not link 2 and 0x5/0x6 are
 * not "links 0+2" / "links 1+2". Every case below goes through the table.
 */
#include <stdio.h>
#include <string.h>
#define BIT_(n) (1u << (n))
#include "mlo_policy_modes.h"

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

/* link ids as negotiated in this project: L0=2.4, L1=5, L2=6 */
static struct mt7925_mlo_mode_ctx ctx_base(void)
{
	struct mt7925_mlo_mode_ctx c;

	memset(&c, 0, sizeof(c));
	c.band_link[MT7925_MLO_BAND_2G] = 0;
	c.band_link[MT7925_MLO_BAND_5G] = 1;
	c.band_link[MT7925_MLO_BAND_6G] = 2;
	c.band_present = BIT_(MT7925_MLO_BAND_2G) | BIT_(MT7925_MLO_BAND_5G) |
			 BIT_(MT7925_MLO_BAND_6G);
	return c;
}

int main(void)
{
	struct mt7925_mlo_mode_ctx c = ctx_base();
	unsigned int links;

	/* A: ids are distinct custom identifiers */
	check("A_ids_3", MT7925_MLO_MODE_SIM_24_5, 0x3);
	check("A_ids_4", MT7925_MLO_MODE_SIM_24_6, 0x4);
	check("A_ids_5", MT7925_MLO_MODE_EMLSR_5, 0x5);
	check("A_ids_6", MT7925_MLO_MODE_EMLSR_6, 0x6);

	/* B: simultaneous modes resolve to link bitmaps from discovered bands */
	check("B_sim245_ok", mt7925_mlo_mode_eval(MT7925_MLO_MODE_SIM_24_5, &c, &links),
	      MT7925_MLO_MODE_OK);
	check("B_sim245_links", links, 0x3);
	check("B_sim246_ok", mt7925_mlo_mode_eval(MT7925_MLO_MODE_SIM_24_6, &c, &links),
	      MT7925_MLO_MODE_OK);
	check("B_sim246_links_is_0x5_not_mode_id", links, 0x5);

	/* C: mapping follows discovery, not the project's usual numbering */
	c = ctx_base();
	c.band_link[MT7925_MLO_BAND_2G] = 2;
	c.band_link[MT7925_MLO_BAND_5G] = 0;
	c.band_link[MT7925_MLO_BAND_6G] = 1;
	check("C_remapped_sim245", mt7925_mlo_mode_eval(MT7925_MLO_MODE_SIM_24_5, &c, &links),
	      MT7925_MLO_MODE_OK);
	check("C_remapped_sim245_links", links, 0x5);
	check("C_remapped_sim246", mt7925_mlo_mode_eval(MT7925_MLO_MODE_SIM_24_6, &c, &links),
	      MT7925_MLO_MODE_OK);
	check("C_remapped_sim246_links", links, 0x6);

	/* D: a band that was not negotiated makes the mode unavailable */
	c = ctx_base();
	c.band_present &= ~BIT_(MT7925_MLO_BAND_6G);
	check("D_no_6g", mt7925_mlo_mode_eval(MT7925_MLO_MODE_SIM_24_6, &c, &links),
	      MT7925_MLO_MODE_BAND_ABSENT);
	check("D_no_6g_links_zero", links, 0);
	check("D_5g_still_ok", mt7925_mlo_mode_eval(MT7925_MLO_MODE_SIM_24_5, &c, &links),
	      MT7925_MLO_MODE_OK);

	/* E: no simultaneous mode may hold two links on the same RF slot */
	c = ctx_base();
	check("E_slots_distinct_245", mt7925_mlo_mode_slots_distinct(MT7925_MLO_MODE_SIM_24_5), 1);
	check("E_slots_distinct_246", mt7925_mlo_mode_slots_distinct(MT7925_MLO_MODE_SIM_24_6), 1);
	check("E_5_plus_6_is_one_slot",
	      mt7925_mlo_band_slot(MT7925_MLO_BAND_5G) == mt7925_mlo_band_slot(MT7925_MLO_BAND_6G), 1);
	check("E_no_mode_is_simultaneous_5_6", mt7925_mlo_mode_simultaneous_high_band(), 0);

	/* F: EMLSR modes are unavailable unless EVERY prerequisite is real */
	c = ctx_base();
	check("F_default_unavailable_5", mt7925_mlo_mode_eval(MT7925_MLO_MODE_EMLSR_5, &c, &links),
	      MT7925_MLO_MODE_AP_NO_EMLSR);
	check("F_default_unavailable_6", mt7925_mlo_mode_eval(MT7925_MLO_MODE_EMLSR_6, &c, &links),
	      MT7925_MLO_MODE_AP_NO_EMLSR);
	check("F_links_zero_when_unavailable", links, 0);

	c.ap_emlsr = 1;
	check("F_ap_only_reports_firmware_as_root_cause",
	      mt7925_mlo_mode_eval(MT7925_MLO_MODE_EMLSR_5, &c, &links),
	      MT7925_MLO_MODE_FW_NO_EMLSR);
	c.fw_emlsr = 1;
	check("F_fw_cap_but_not_advertised",
	      mt7925_mlo_mode_eval(MT7925_MLO_MODE_EMLSR_5, &c, &links),
	      MT7925_MLO_MODE_LOCAL_NO_EMLSR);
	c.local_emlsr_adv = 1;
	check("F_fw_cap_still_needs_switch_primitive",
	      mt7925_mlo_mode_eval(MT7925_MLO_MODE_EMLSR_6, &c, &links),
	      MT7925_MLO_MODE_NO_SWITCH_PRIMITIVE);
	c.switch_primitive_verified = 1;
	check("F_primitive_still_needs_established_composition",
	      mt7925_mlo_mode_eval(MT7925_MLO_MODE_EMLSR_6, &c, &links),
	      MT7925_MLO_MODE_COMPOSITION_UNESTABLISHED);
	check("F_links_zero_without_composition", links, 0);
	c.emlsr_links[0] = 0;
	c.emlsr_links_known = 1;
	c.emlsr_links[0] = 0x6;
	c.emlsr_links[1] = 0x6;
	check("F_all_prereqs_ok_6", mt7925_mlo_mode_eval(MT7925_MLO_MODE_EMLSR_6, &c, &links),
	      MT7925_MLO_MODE_OK);
	check("F_links_from_evidence", links, 0x6);

	/* G: a mode must not be substituted for another */
	check("G_unknown_mode", mt7925_mlo_mode_eval(0x7, &c, &links), MT7925_MLO_MODE_UNKNOWN);
	check("G_zero_mode", mt7925_mlo_mode_eval(0x0, &c, &links), MT7925_MLO_MODE_UNKNOWN);

	/* H: classify the current published link mask against the table */
	c = ctx_base();
	check("H_cur_0x3", mt7925_mlo_mode_of_links(0x3, &c), MT7925_MLO_MODE_SIM_24_5);
	check("H_cur_0x5", mt7925_mlo_mode_of_links(0x5, &c), MT7925_MLO_MODE_SIM_24_6);
	check("H_cur_0x6_is_not_a_mode", mt7925_mlo_mode_of_links(0x6, &c), 0);
	check("H_cur_0x7_is_not_a_mode", mt7925_mlo_mode_of_links(0x7, &c), 0);
	check("H_cur_single_link_is_not_a_mode", mt7925_mlo_mode_of_links(0x4, &c), 0);

	/* I: reason strings exist for every outcome (diagnostics must be readable) */
	check("I_reason_ok", strcmp(mt7925_mlo_mode_reason(MT7925_MLO_MODE_OK), "ok") == 0, 1);
	check("I_reason_primitive",
	      strcmp(mt7925_mlo_mode_reason(MT7925_MLO_MODE_NO_SWITCH_PRIMITIVE),
		     "no_verified_switch_primitive") == 0, 1);
	check("I_reason_bad", strcmp(mt7925_mlo_mode_reason(99), "?") == 0, 1);

	/* J: transition classes. The shared 5/6 GHz radio (slot 1) can serve one
	 * high band at a time, so changing the high band while the 2.4 GHz link
	 * stays is a retune of that radio, not an activation. */
	{
		const int band_of_link[3] = { MT7925_MLO_BAND_2G, MT7925_MLO_BAND_5G, MT7925_MLO_BAND_6G };

		check("J_same_is_none", mt7925_mlo_switch_class(0x3, 0x3, band_of_link), MT7925_MLO_SWITCH_NONE);
		check("J_mode3_to_mode4_is_highband_retune",
		      mt7925_mlo_switch_class(0x3, 0x5, band_of_link), MT7925_MLO_SWITCH_HIGHBAND_RETUNE);
		check("J_mode4_to_mode3_is_highband_retune",
		      mt7925_mlo_switch_class(0x5, 0x3, band_of_link), MT7925_MLO_SWITCH_HIGHBAND_RETUNE);
		check("J_single_link_to_mode_is_add",
		      mt7925_mlo_switch_class(0x4, 0x5, band_of_link), MT7925_MLO_SWITCH_ADD_LINK);
		check("J_single_low_to_mode_is_add",
		      mt7925_mlo_switch_class(0x1, 0x3, band_of_link), MT7925_MLO_SWITCH_ADD_LINK);
		check("J_5_6_mask_to_mode_is_replace",
		      mt7925_mlo_switch_class(0x6, 0x5, band_of_link), MT7925_MLO_SWITCH_REPLACE);
		check("J_5_6_mask_to_mode3_is_replace",
		      mt7925_mlo_switch_class(0x6, 0x3, band_of_link), MT7925_MLO_SWITCH_REPLACE);
		check("J_mode_to_single_is_pure_removal",
		      mt7925_mlo_switch_class(0x5, 0x1, band_of_link), MT7925_MLO_SWITCH_REMOVE);
	}
	/* K: a class is attempted only when it is verified in this association,
	 * or an experiment explicitly asks for unverified attempts. */
	check("K_none_always_ok", mt7925_mlo_switch_feasible(MT7925_MLO_SWITCH_NONE, 0, 0), 1);
	/* a shrink needs no acquisition: it is the one step proven safe live */
	check("K_pure_removal_always_ok", mt7925_mlo_switch_feasible(MT7925_MLO_SWITCH_REMOVE, 0, 0), 1);
	check("K_unverified_blocked", mt7925_mlo_switch_feasible(MT7925_MLO_SWITCH_HIGHBAND_RETUNE, 0, 0), 0);
	check("K_other_class_verified_does_not_help",
	      mt7925_mlo_switch_feasible(MT7925_MLO_SWITCH_HIGHBAND_RETUNE,
					 1u << MT7925_MLO_SWITCH_ADD_LINK, 0), 0);
	check("K_verified_class_ok",
	      mt7925_mlo_switch_feasible(MT7925_MLO_SWITCH_HIGHBAND_RETUNE,
					 1u << MT7925_MLO_SWITCH_HIGHBAND_RETUNE, 0), 1);
	check("K_experiment_override",
	      mt7925_mlo_switch_feasible(MT7925_MLO_SWITCH_ADD_LINK, 0, 1), 1);
	check("K_class_names", strcmp(mt7925_mlo_switch_class_name(MT7925_MLO_SWITCH_HIGHBAND_RETUNE),
				      "highband_retune") == 0, 1);

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures ? 1 : 0;
}
