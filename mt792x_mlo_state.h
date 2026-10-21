/* SPDX-License-Identifier: BSD-3-Clause-Clear */

#ifndef __MT792X_MLO_STATE_H
#define __MT792X_MLO_STATE_H

#include <net/mac80211.h>

struct mt792x_dev;
struct mt792x_sta;

struct mt7927_mlo_link_state {
	bool negotiated;
	bool resident;
	bool traffic_active;
	bool link_ready;
	bool ptk_synced;
	bool group_synced;
	bool removing;
	bool adding;
	/* Set by mt7927_mlo_reassign_band() when this transition really moved the
	 * link's DEV/BSS to another band slot; consumed by
	 * mt7927_mlo_refresh_reassigned_sta() so a peer whose band did not move
	 * is not destructively recreated.
	 */
	bool band_moved;
	/* Explicit ROC/admission state, owned by mt7927_mlo_change_vif_links().
	 * A link only becomes traffic-eligible in
	 * mt7927_mlo_change_sta_links() when this is true AND
	 * admitted_generation matches the vif's current
	 * mt7927_mlo_state.transition_generation -- never inferred from
	 * callback ordering, new_links, `created`, link_ready, or the
	 * transient global phy->roc_grant (which is already consumed/reset
	 * by the time the STA callback runs). The generation check exists so
	 * a stale, delayed grant/timeout event from an earlier transition can
	 * never authorize a link_id that has since been retired and
	 * re-added.
	 */
	bool roc_admitted;
	u32 admitted_generation;
	u8 link_id;
	u8 bss_idx;
	u8 fw_context_slot;
	u8 omac_idx;
	u8 wmm_idx;
	u16 peer_wcid;
	u16 bmc_wcid;
};

struct mt7927_mlo_state {
	/* last reported availability of each steering mode (avail + 1; 0 = unreported) */
	u8 mode_avail_noted[4];
	u8 mode_block_noted[4];
	/* transition classes that completed with authenticated validation in the
	 * association identified by steer_state_epoch (assoc_epoch + 1; 0 = unset)
	 */
	u8 verified_switch_classes;
	u64 steer_state_epoch;
	u16 negotiated_links;
	u16 resident_links;
	u16 traffic_active_links;
	/* Post-regrant security rebind (test gate mlo_security_rebind): the
	 * post-grant active set the STA_REC_MLD record must describe while
	 * mac80211 has not yet published it, and the radio-group reincarnation
	 * flag that marks every active link's firmware security binding stale.
	 */
	u16 rebind_target_links;
	bool radio_group_reincarnated;
	u8 primary_link_id;
	u8 secondary_link_id;
	u32 transition_generation;
	/* Assoc-time JOIN deferral (mlo_join_wait_bss): set when
	 * mt7925_mac_set_links() found the partner link BSS unregistered and
	 * left the JOIN to mt7927_mlo_change_vif_links(); consumed there (or
	 * dropped on teardown). join_deferred_links is the pair it wanted.
	 */
	bool join_deferred;
	/* mlo_activate_after_auth: link activation requested at STA ASSOC is held
	 * until the STA is authorized (4-way done). */
	bool activate_deferred;
	unsigned long activate_deferred_jiffies;
	u16 join_deferred_links;
	u32 join_deferred_gen;
	u16 migration_from_links;
	u16 migration_to_links;
	/* Production pair transition (mlo_pair_transition.h). The transition
	 * id is transition_generation; a completion for another generation
	 * never mutates this record.
	 */
	u8 pt_state;
	u8 pt_kind;
	u8 pt_token;
	s8 pt_link;
	u8 pt_req_slot;
	u8 pt_grant_slot;
	u16 pt_old_links;
	u16 pt_target_links;
	u16 pt_survivor;
	u16 pt_removed;
	u16 pt_added;
	/* Monotonic ns stamps of one transition (0 = not reached):
	 * 0 requested, 1 survivor ownership released, 2 survivor re-JOIN sent,
	 * 3 survivor grant, 4 survivor recovery started, 5 first survivor
	 * secure RX, 6 incoming ACQUIRE sent, 7 incoming grant, 8 first
	 * incoming secure RX, 9 converged.
	 */
	u64 pt_ns[10];
	struct mt7927_mlo_link_state link[IEEE80211_MLD_MAX_NUM_LINKS];
};

void mt7925_mlo_sample_load(struct mt792x_dev *dev);
u32 mt7925_mlo_dbg_elapsed(u8 link_id);
u32 mt7925_mlo_dbg_raw_util(u8 link_id);
u32 mt7925_mlo_dbg_util(u8 link_id);
u32 mt7925_mlo_dbg_delta(u8 link_id);
u32 mt7925_mlo_dbg_spare(u8 link_id);
u32 mt7925_mlo_dbg_weight(u8 link_id);
u64 mt7925_mlo_dbg_placements(u8 link_id);
u64 mt7925_mlo_dbg_cache_hits(u8 link_id);
void mt7925_mlo_clear_tid_link(struct mt792x_sta *msta, int tid);

#endif /* __MT792X_MLO_STATE_H */
