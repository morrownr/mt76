/* SPDX-License-Identifier: ISC */
/* Standalone contract test; plain userspace C, no kernel headers. */
#include <stdio.h>
#include "mlo_roc_delta.h"
#include "mlo_roc_state.h"

static int failures;

static void check(const char *name, unsigned int got, unsigned int want)
{
	if (got != want) {
		printf("FAIL %s: got=0x%x want=0x%x\n", name, got, want);
		failures++;
	} else {
		printf("PASS %s\n", name);
	}
}

static void held(const char *n, struct mt7925_mlo_roc_state *s,
		 unsigned int tok, unsigned int links)
{
	char b[80];

	snprintf(b, sizeof(b), "%s_held_token", n);
	check(b, s->held_token, tok);
	snprintf(b, sizeof(b), "%s_held_links", n);
	check(b, s->held_links, links);
}

static void pend(const char *n, struct mt7925_mlo_roc_state *s,
		 unsigned int tok, unsigned int links)
{
	char b[80];

	snprintf(b, sizeof(b), "%s_pending_token", n);
	check(b, s->pending_token, tok);
	snprintf(b, sizeof(b), "%s_pending_links", n);
	check(b, s->pending_links, links);
}

/* Held JOIN token 3 over @old, then the mac80211 shrink phase releases the
 * removed slot only.
 */
static void shrink(struct mt7925_mlo_roc_state *s, unsigned int old,
		   unsigned int new, unsigned int *released)
{
	unsigned int mask = mt7925_mlo_roc_release_mask(old, new, 0x7,
							s->held_links);

	*released = mt7925_mlo_roc_state_release(s, mask);
}

static void swap(const char *name, unsigned int old, unsigned int new,
		 unsigned int rem, unsigned int surv, unsigned int add)
{
	struct mt7925_mlo_roc_state s = {};
	unsigned int rel, tok, mask;
	char n[80];

	/* CASE 1: initial grant */
	mt7925_mlo_roc_state_request(&s, 3, old);
	pend("case1_request", &s, 3, old);
	held("case1_request", &s, 0, 0);
	check("case1_no_anomaly", mt7925_mlo_roc_state_grant(&s), 0);
	held("case1_grant", &s, 3, old);
	pend("case1_grant", &s, 0, 0);

	/* CASE 2: selective release keeps token and survivor */
	shrink(&s, old, surv, &rel);
	snprintf(n, sizeof(n), "%s_released", name);
	check(n, rel, rem);
	snprintf(n, sizeof(n), "%s_case2", name);
	held(n, &s, 3, surv);

	/* CASE 3: replacement pending: held untouched, pending exact */
	mt7925_mlo_roc_state_request(&s, 4, new);
	snprintf(n, sizeof(n), "%s_case3", name);
	held(n, &s, 3, surv);
	pend(n, &s, 4, new);

	/* CASE 4: timeout cancels the request under ITS OWN token 4 (every
	 * slot it named, so a queued grant cannot arrive later); the held
	 * token-3 survivor slot is untouched.
	 */
	mask = mt7925_mlo_roc_state_timeout(&s, &tok);
	snprintf(n, sizeof(n), "%s_timeout_cancels_request_slots", name);
	check(n, mask, new);
	(void)add;
	snprintf(n, sizeof(n), "%s_timeout_token", name);
	check(n, tok, 4);
	snprintf(n, sizeof(n), "%s_case4", name);
	held(n, &s, 3, surv);
	pend(n, &s, 0, 0);

	/* CASE 6: teardown after the failed transition frees the survivor
	 * under its OWNING token.
	 */
	mask = mt7925_mlo_roc_state_release_all(&s, &tok);
	snprintf(n, sizeof(n), "%s_teardown_mask", name);
	check(n, mask, surv);
	snprintf(n, sizeof(n), "%s_teardown_token", name);
	check(n, tok, 3);
	snprintf(n, sizeof(n), "%s_case6", name);
	held(n, &s, 0, 0);
}


static void attach_tests(void)
{
	struct mt7925_mlo_roc_state s = {};
	unsigned int tok, mask;

	/* 1. existing held state: token 3 / survivor link2 (0x4) */
	mt7925_mlo_roc_state_request(&s, 3, 0x6);
	mt7925_mlo_roc_state_grant(&s);
	mt7925_mlo_roc_state_release(&s, 0x2);
	held("attach1", &s, 3, 0x4);

	/* 2. attach request: link0 (0x1, bss 2) under the HELD token 3 */
	mt7925_mlo_roc_state_attach_request(&s, 3, 0x1, 1u << 2);
	held("attach2", &s, 3, 0x4);
	check("attach2_pending_token", s.attach_pending_token, 3);
	check("attach2_pending_links", s.attach_pending_links, 0x1);

	/* 6/7. wrong token, wrong bss (the survivor's own bss 0): no promote */
	check("attach_wrong_token", mt7925_mlo_roc_state_attach_grant(&s, 4, 2), 0);
	check("attach_wrong_bss", mt7925_mlo_roc_state_attach_grant(&s, 3, 0), 0);
	held("attach_no_promote", &s, 3, 0x4);
	check("attach_still_pending", s.attach_pending_links, 0x1);

	/* 3. matching grant: held 3 / 0x5, attach pending cleared */
	check("attach3_match", mt7925_mlo_roc_state_attach_grant(&s, 3, 2), 1);
	held("attach3", &s, 3, 0x5);
	check("attach3_cleared_token", s.attach_pending_token, 0);
	check("attach3_cleared_links", s.attach_pending_links, 0);
	check("attach3_duplicate_ignored", mt7925_mlo_roc_state_attach_grant(&s, 3, 2), 0);

	/* 8. teardown after a successful attach frees both slots under 3 */
	mask = mt7925_mlo_roc_state_release_all(&s, &tok);
	check("attach8_mask", mask, 0x5);
	check("attach8_token", tok, 3);
	held("attach8", &s, 0, 0);

	/* 4. attach timeout: held stays 3 / 0x4, only the added slot cancelled */
	mt7925_mlo_roc_state_request(&s, 3, 0x6);
	mt7925_mlo_roc_state_grant(&s);
	mt7925_mlo_roc_state_release(&s, 0x2);
	mt7925_mlo_roc_state_attach_request(&s, 3, 0x1, 1u << 2);
	mask = mt7925_mlo_roc_state_attach_timeout(&s, &tok);
	check("attach4_cancel_links_added_only", mask, 0x1);
	check("attach4_cancel_token", tok, 3);
	held("attach4", &s, 3, 0x4);
	check("attach4_pending_cleared", s.attach_pending_links, 0);

	/* 5. late attach grant: only the added slot becomes an orphan */
	check("attach5_late_survivor_bss_ignored",
	      mt7925_mlo_roc_state_attach_late_grant(&s, 3, 0), 0);
	check("attach5_late_wrong_token_ignored",
	      mt7925_mlo_roc_state_attach_late_grant(&s, 4, 2), 0);
	check("attach5_late_added_slot",
	      mt7925_mlo_roc_state_attach_late_grant(&s, 3, 2), 1);
	check("attach5_orphan_token", s.orphan_token, 3);
	check("attach5_orphan_bss_added_only", s.orphan_bss, 1u << 2);
	held("attach5_survivor_untouched", &s, 3, 0x4);
	check("attach5_orphan_take", mt7925_mlo_roc_state_orphan_take(&s, &tok), 1u << 2);
	check("attach5_orphan_token_taken", tok, 3);

	/* 9. teardown after a failed attach frees only the survivor slot */
	mask = mt7925_mlo_roc_state_release_all(&s, &tok);
	check("attach9_mask", mask, 0x4);
	check("attach9_token", tok, 3);

	/* release_all also drops an unresolved attach request */
	mt7925_mlo_roc_state_request(&s, 3, 0x6);
	mt7925_mlo_roc_state_grant(&s);
	mt7925_mlo_roc_state_attach_request(&s, 3, 0x1, 1u << 2);
	mt7925_mlo_roc_state_release_all(&s, &tok);
	check("attach_release_all_clears_pending", s.attach_pending_token, 0);
	/* a fresh request forgets the timed-out attach identity */
	mt7925_mlo_roc_state_request(&s, 3, 0x6);
	mt7925_mlo_roc_state_grant(&s);
	mt7925_mlo_roc_state_attach_request(&s, 3, 0x1, 1u << 2);
	mt7925_mlo_roc_state_attach_timeout(&s, &tok);
	mt7925_mlo_roc_state_attach_request(&s, 3, 0x2, 1u << 1);
	check("attach_new_request_supersedes_late",
	      mt7925_mlo_roc_state_attach_late_grant(&s, 3, 2), 0);
}

int main(void)
{
	struct mt7925_mlo_roc_state s = {};
	unsigned int tok, mask, rel;

	swap("0x6_to_0x5", 0x6, 0x5, 0x2, 0x4, 0x1);
	swap("0x5_to_0x3", 0x5, 0x3, 0x4, 0x1, 0x2);
	swap("0x3_to_0x6", 0x3, 0x6, 0x1, 0x2, 0x4);
	swap("0x6_to_0x3", 0x6, 0x3, 0x4, 0x2, 0x1);
	swap("0x3_to_0x5", 0x3, 0x5, 0x2, 0x1, 0x4);
	swap("0x5_to_0x6", 0x5, 0x6, 0x1, 0x4, 0x2);

	/* CASE 5: normal replacement grant after a release-all */
	mt7925_mlo_roc_state_request(&s, 3, 0x6);
	mt7925_mlo_roc_state_grant(&s);
	mask = mt7925_mlo_roc_state_release_all(&s, &tok);
	check("case5_release_all_mask", mask, 0x6);
	mt7925_mlo_roc_state_request(&s, 4, 0x5);
	held("case5_pending", &s, 0, 0);
	check("case5_timeout_mask_full", mt7925_mlo_roc_state_timeout(&s, &tok), 0x5);
	check("case5_timedout_token_recorded", s.timedout_token, 4);
	mt7925_mlo_roc_state_request(&s, 4, 0x5);
	check("case5_no_anomaly", mt7925_mlo_roc_state_grant(&s), 0);
	held("case5_grant", &s, 4, 0x5);
	pend("case5_grant", &s, 0, 0);

	/* CASE 7: association teardown frees all held slots, then nothing */
	mask = mt7925_mlo_roc_state_release_all(&s, &tok);
	check("case7_mask", mask, 0x5);
	check("case7_token", tok, 4);
	check("case7_second_release_empty",
	      mt7925_mlo_roc_state_release_all(&s, &tok), 0);
	held("case7", &s, 0, 0);

	/* CASE 8: zero-sided operations keep release-everything, and a
	 * release of a slot that is not held is a no-op.
	 */
	mt7925_mlo_roc_state_request(&s, 7, 0x6);
	mt7925_mlo_roc_state_grant(&s);
	check("case8_teardown_mask", mt7925_mlo_roc_release_mask(0x6, 0x0, 0x7, s.held_links), 0x6);
	rel = mt7925_mlo_roc_state_release(&s, 0x1);
	check("case8_unheld_release_noop", rel, 0);
	held("case8_after_noop", &s, 7, 0x6);
	rel = mt7925_mlo_roc_state_release(&s, 0x6);
	check("case8_full_release", rel, 0x6);
	held("case8_full_release", &s, 0, 0);

	/* A grant for a different token while other slots are still held is
	 * reported so it is never silent.
	 */
	mt7925_mlo_roc_state_request(&s, 3, 0x6);
	mt7925_mlo_roc_state_grant(&s);
	mt7925_mlo_roc_state_release(&s, 0x2);
	mt7925_mlo_roc_state_request(&s, 4, 0x5);
	check("grant_over_held_anomaly", mt7925_mlo_roc_state_grant(&s), 1);

	/* Same-token grant merges without an anomaly. */
	mt7925_mlo_roc_state_release_all(&s, &tok);
	mt7925_mlo_roc_state_request(&s, 3, 0x6);
	mt7925_mlo_roc_state_grant(&s);
	mt7925_mlo_roc_state_release(&s, 0x2);
	mt7925_mlo_roc_state_request(&s, 3, 0x1);
	check("same_token_grant_no_anomaly", mt7925_mlo_roc_state_grant(&s), 0);
	held("same_token_merge", &s, 3, 0x5);

	/* LATE GRANT: firmware grants a timed-out request after the fact
	 * (observed live: token 4 granted 28 s after its timeout, right after
	 * the survivor's token-3 slot was released). It must be recorded as an
	 * orphan and released under ITS token, and must not be mistaken for a
	 * grant of the current request.
	 */
	mt7925_mlo_roc_state_release_all(&s, &tok);
	mt7925_mlo_roc_state_request(&s, 3, 0x6);
	mt7925_mlo_roc_state_grant(&s);
	mt7925_mlo_roc_state_release(&s, 0x2);
	mt7925_mlo_roc_state_request(&s, 4, 0x5);
	mt7925_mlo_roc_state_timeout(&s, &tok);
	check("late_unrelated_token_ignored", mt7925_mlo_roc_state_late_grant(&s, 9, 0), 0);
	check("late_grant_recognised", mt7925_mlo_roc_state_late_grant(&s, 4, 0), 1);
	check("late_grant_second_slot", mt7925_mlo_roc_state_late_grant(&s, 4, 2), 1);
	check("late_grant_orphan_token", s.orphan_token, 4);
	check("late_grant_orphan_bss", s.orphan_bss, 0x5);
	held("late_grant_held_untouched", &s, 3, 0x4);
	check("orphan_take_mask", mt7925_mlo_roc_state_orphan_take(&s, &tok), 0x5);
	check("orphan_take_token", tok, 4);
	check("orphan_take_twice_empty", mt7925_mlo_roc_state_orphan_take(&s, &tok), 0);
	/* once taken, a further late slot for the same token is still an orphan */
	check("late_after_take", mt7925_mlo_roc_state_late_grant(&s, 4, 1), 1);
	/* a new request supersedes the cancelled-token memory */
	mt7925_mlo_roc_state_request(&s, 5, 0x5);
	check("late_superseded_ignored", mt7925_mlo_roc_state_late_grant(&s, 4, 1), 0);

	attach_tests();

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures != 0;
}
