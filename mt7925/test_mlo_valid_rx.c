/* Standalone contract test for MLO valid-RX evidence and late readiness
 * rearm. Build and run:
 *   gcc -Wall -Wextra -Werror -o /tmp/test_mlo_valid_rx test_mlo_valid_rx.c && /tmp/test_mlo_valid_rx
 */
#include <stdio.h>
#include "mlo_valid_rx.h"
#include "mlo_link_readiness.h"

#define STABILIZE_TICKS 500

static int failures;

static void check(const char *name, int got, int want)
{
	if (got != want) {
		printf("FAIL %s: got=%d want=%d\n", name, got, want);
		failures++;
	} else {
		printf("PASS %s\n", name);
	}
}

static struct mt7925_mlo_rx_evidence good_data(void)
{
	struct mt7925_mlo_rx_evidence e = {
		.has_wcid = 1, .unicast = 1, .is_data = 1, .protected = 1,
		.decrypted = 1, .peer_keyed = 1,
	};
	return e;
}

static struct mt7925_mlo_rearm_state waiting(void)
{
	struct mt7925_mlo_rearm_state s = {
		.link_valid = 1, .in_active_links = 1, .awaiting_valid_rx = 1,
		.await_generation = 7, .cur_generation = 7, .ready_ts = 0,
	};
	return s;
}

int main(void)
{
	struct mt7925_mlo_rx_evidence e;
	struct mt7925_mlo_rearm_state s;

	e = good_data();
	check("A_decrypted_unicast_data_is_valid",
	      mt7925_mlo_rx_is_valid_datapath(&e), 1);

	e = good_data(); e.wrong_channel = 1;
	check("W1_valid_looking_data_from_another_channel_is_not_link_proof",
	      mt7925_mlo_rx_is_valid_datapath(&e), 0);

	e = good_data(); e.wrong_channel = 0;
	check("W2_same_channel_data_still_valid",
	      mt7925_mlo_rx_is_valid_datapath(&e), 1);

	e = good_data(); e.cm = 1; e.decrypted = 0;
	check("B_cipher_mismatch_is_invalid",
	      mt7925_mlo_rx_is_valid_datapath(&e), 0);

	e = good_data(); e.icv_err = 1;
	check("C_icv_error_is_invalid_even_if_decrypted_flag_set",
	      mt7925_mlo_rx_is_valid_datapath(&e), 0);

	e = good_data(); e.decrypted = 0;
	check("D_protected_undecrypted_is_invalid",
	      mt7925_mlo_rx_is_valid_datapath(&e), 0);

	e = good_data(); e.is_data = 0;
	check("E_management_or_control_is_invalid",
	      mt7925_mlo_rx_is_valid_datapath(&e), 0);
	e = good_data(); e.is_null_data = 1;
	check("E_null_data_is_invalid",
	      mt7925_mlo_rx_is_valid_datapath(&e), 0);
	e = good_data(); e.unicast = 0;
	check("E_group_addressed_is_invalid",
	      mt7925_mlo_rx_is_valid_datapath(&e), 0);
	e = good_data(); e.fcs_err = 1;
	check("reject_fcs_error", mt7925_mlo_rx_is_valid_datapath(&e), 0);
	e = good_data(); e.tkip_mic_err = 1;
	check("reject_tkip_mic_error", mt7925_mlo_rx_is_valid_datapath(&e), 0);
	e = good_data(); e.protected = 0; e.decrypted = 0;
	check("reject_unprotected_data_on_keyed_peer",
	      mt7925_mlo_rx_is_valid_datapath(&e), 0);

	s = waiting();
	check("F_late_valid_rx_rearms_waiting_link",
	      mt7925_mlo_should_rearm_on_valid_rx(&s, 1), 1);

	s = waiting();
	e = good_data(); e.icv_err = 1;
	check("G_late_invalid_rx_does_not_rearm",
	      mt7925_mlo_should_rearm_on_valid_rx(&s,
			mt7925_mlo_rx_is_valid_datapath(&e)), 0);

	s = waiting(); s.link_valid = 0; s.awaiting_valid_rx = 0;
	check("H_quiesced_removed_link_not_rearmed",
	      mt7925_mlo_should_rearm_on_valid_rx(&s, 1), 0);
	s = waiting(); s.in_active_links = 0;
	check("H_link_outside_active_topology_not_rearmed",
	      mt7925_mlo_should_rearm_on_valid_rx(&s, 1), 0);

	s = waiting(); s.await_generation = 6;
	check("I_stale_generation_not_rearmed",
	      mt7925_mlo_should_rearm_on_valid_rx(&s, 1), 0);

	s = waiting(); s.ready_ts = 1234;
	check("already_ready_not_restamped",
	      mt7925_mlo_should_rearm_on_valid_rx(&s, 1), 0);

	{
		unsigned long rearm_at = 10000;

		check("J_selector_withholds_during_stabilize_after_rearm",
		      mt7925_mlo_link_tx_ready(rearm_at,
					       rearm_at + STABILIZE_TICKS - 1,
					       STABILIZE_TICKS), 0);
		check("J_selector_eligible_after_rearm_and_stabilize",
		      mt7925_mlo_link_tx_ready(rearm_at,
					       rearm_at + STABILIZE_TICKS + 1,
					       STABILIZE_TICKS), 1);
		check("J_never_rearmed_link_stays_ineligible",
		      mt7925_mlo_link_tx_ready(0, rearm_at + 100000,
					       STABILIZE_TICKS), 0);
	}

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}
