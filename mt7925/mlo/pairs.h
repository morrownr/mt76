/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7927_MLO_PAIRS_H
#define __MT7927_MLO_PAIRS_H

#include "../mt7925.h"
#include "../mlo_recovery_policy.h"
#include "../mlo_policy_modes.h"
#include "../mlo_severity.h"

enum mt7927_mlo_support {
	MT7927_MLO_SUPPORT_UNKNOWN,
	MT7927_MLO_SUPPORT_SUPPORTED,
	MT7927_MLO_SUPPORT_UNSUPPORTED,
};

enum mt7927_mlo_control {
	MT7927_MLO_CTL_PAIR03,
	MT7927_MLO_CTL_PAIR05,
	MT7927_MLO_CTL_PAIR06,
	MT7927_MLO_CTL_ENABLE,
	MT7927_MLO_CTL_POLICY,
	MT7927_MLO_CTL_INTERVAL,
	MT7927_MLO_CTL_FALLBACK,
};

struct mt7927_mlo_pair_failure {
	u64 ns;
	u64 epoch;
	u64 observation_generation;
	int error;
	char reason[64];
};

/* Failure backoff for a steering target: base is the steering cooldown, and
 * it doubles per consecutive failure up to 2^MT7927_MLO_BACKOFF_MAX_SHIFT.
 * Only REAL transition failures count (refusal, rollback); a request that was
 * never started does not.
 */
#define MT7927_MLO_BACKOFF_BASE_MS	20000ULL
#define MT7927_MLO_BACKOFF_MAX_SHIFT	4

struct mt7927_mlo_pair_profile {
	u16 mask;
	bool enabled;
	enum mt7927_mlo_support support;
	/* No debugfs setter. Host prerequisites never certify TCP operation. */
	bool current_validated;
	u32 historical_memory;
	/* Authentic prior-path observations, including failures; not acceptance. */
	u32 prior_implementation_memory;
	int last_error;
	u32 failures;
	u32 consecutive_failures;
	u64 backoff_until_ns;
	u64 last_attempt_ns;
	u64 last_attempt_epoch;
	u64 last_core_activation_ns;
	u64 last_core_activation_epoch;
	char last_reason[64];
	struct mt7927_mlo_pair_failure failure[4];
};

struct mt7927_mlo_profiles {
	bool device_supported;
	u32 interval_ms;
	u32 policy;
	u16 fallback_pair;
	u64 observation_generation;
	u64 association_epoch;
	bool authorized;
	struct mt7927_mlo_pair_profile pair[3];
};

/* A replacement link failed admission inside a change_sta_links callback.
 * mac80211 ignores that callback's error and would publish the requested
 * mask, so the completion owner restores the survivor mask after the outer
 * setter returns. Protected by mt792x_vif.mlo_completion.lock.
 */
struct mt7925_mlo_admit_fail {
	bool pending;
	u16 failed;
	u16 prev;
	u16 survivor;
	int error;
	u64 epoch;
};

/* Kernel integration: extend this driver's private VIF allocation only.
 * The unchanged mt792x prefix remains at offset zero; shared modules neither
 * allocate nor access the extension. No shared structure or key store grows.
 */
/* Post-failed-admission recovery watch (mlo/recovery.c). The decision state is
 * the pure policy; the rest is the worker plumbing. Private VIF extension only.
 */
struct mt7925_mlo_recovery_state {
	struct delayed_work work;
	bool initialized;
	bool reset_seen;
	bool reassoc_req;	/* worker: leave cleanly instead of watching */
	bool reassoc_seen;
	u64 last_reassoc_ms;
	u64 reassoc_epoch;	/* association the request was made in */
	u8 link;
	u16 failed;
	u16 prev;
	u64 last_reset_ms;
	struct mt7925_mlo_recovery policy;
};

/* Severity memory and the first genuine 0x7 band map (mlo/severity.c). Guarded by
 * pair_lock; lives in the private VIF so it survives a firmware reset.
 */
struct mt7925_mlo_severity_state {
	struct mt7925_mlo_lastgood lastgood;
	struct mt7925_mlo_sev sev;
};

struct mt7925_mlo_vif {
	struct mt792x_vif base;
	spinlock_t pair_lock;
	/* Steering is wanted for this association (module default or operator).
	 * Foreign link/channel changes clear the running flag; this re-arms it.
	 */
	bool auto_intent;
	struct mt7925_mlo_admit_fail admit_fail;
	struct mt7925_mlo_recovery_state recovery;
	struct mt7925_mlo_severity_state severity;
	struct mt7927_mlo_profiles profiles;
};

int mt7927_mlo_pair_index(u16 mask);
bool mt7927_mlo_context_link_matches(unsigned int requested, int actual);
void mt7927_mlo_profiles_init(struct mt7927_mlo_profiles *p, bool mt7927);
void mt7927_mlo_profiles_invalidate(struct mt7927_mlo_profiles *p);
bool mt7927_mlo_profiles_can_steer(const struct mt7927_mlo_profiles *p);
int mt7927_mlo_profiles_admit(const struct mt7927_mlo_profiles *p, u16 target,
			      u16 valid, u16 usable, bool associated, bool negotiate);
int mt7927_mlo_pairs_admit(struct ieee80211_vif *vif, u16 target, bool negotiate);
int mt7927_mlo_pairs_prepare_association(struct ieee80211_vif *vif, u16 target);
int mt7927_mlo_profiles_configure(struct mt7927_mlo_profiles *p,
				enum mt7927_mlo_control control, u64 value);
void mt7927_mlo_profile_note(struct mt7927_mlo_profiles *profiles, u16 target,
			     int error, const char *reason, u64 now, bool core_done);
void mt7927_mlo_pairs_init(struct ieee80211_vif *vif);
void mt7927_mlo_pairs_invalidate(struct ieee80211_vif *vif, bool security);
void mt7927_mlo_pairs_authorized(struct ieee80211_vif *vif);
void mt7927_mlo_pairs_autostart(struct ieee80211_vif *vif);
void mt7927_mlo_pairs_note(struct ieee80211_vif *vif, u16 target,
			   int error, const char *reason, bool core_done);
void mt7925_mlo_completion_admission_failed(struct ieee80211_vif *vif,
					  u16 old_links, u16 new_links,
					  u16 prev_links, int error);
void mt7925_mlo_recovery_init(struct ieee80211_vif *vif);
void mt7925_mlo_recovery_arm_survivor(struct ieee80211_vif *vif, u16 survivors, u16 failed, u16 prev, u64 epoch);
void mt7925_mlo_recovery_assoc_lost(struct ieee80211_vif *vif);
void mt7925_mlo_recovery_request_reassoc(struct ieee80211_vif *vif, u16 prev, u16 failed);
bool mt7925_mlo_recovery_reassoc_pending(struct ieee80211_vif *vif);
void mt7925_mlo_build_mode_ctx(struct ieee80211_vif *vif, struct mt792x_vif *mvif,
			       struct mt7925_mlo_mode_ctx *c);
void mt7925_mlo_mode_band_of_link(const struct mt7925_mlo_mode_ctx *c, int band_of_link[3]);
void mt7925_mlo_severity_lastgood_note(struct ieee80211_vif *vif);
void mt7925_mlo_severity_mark_previous(struct ieee80211_vif *vif, u16 prev_links);
u16 mt7925_mlo_severity_decide(struct ieee80211_vif *vif, unsigned long usable_links);
int mt7925_mlo_band_index(enum nl80211_band band);
enum nl80211_band mt7925_mlo_link_band(struct ieee80211_vif *vif, int link_id);
bool mt7925_mlo_severity_blocked(struct ieee80211_vif *vif, u16 links,
				 const int band_of_link[3]);
void mt7925_mlo_severity_post_reset(struct ieee80211_vif *vif);
void mt7925_mlo_severity_show(struct seq_file *s, struct ieee80211_vif *vif);
void mt7925_mlo_recovery_stop(struct ieee80211_vif *vif);
bool mt7927_mlo_pairs_can_steer(struct ieee80211_vif *vif);
bool mt7927_mlo_pair_enabled(struct ieee80211_vif *vif, u16 mask);
bool mt7927_mlo_pair_backoff_active(struct ieee80211_vif *vif, u16 mask);
unsigned long mt7927_mlo_steering_interval(struct ieee80211_vif *vif);
void mt7927_mlo_steering_init(struct ieee80211_vif *vif);
void mt7927_mlo_steering_kick(struct ieee80211_vif *vif);
void mt7927_mlo_steering_rssi_event(struct ieee80211_vif *vif, s32 rssi);
int mt7927_mlo_pairs_debugfs_init(struct mt792x_dev *dev, struct dentry *dir);

#endif
