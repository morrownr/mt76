/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef __MT7925_MLO_COMPLETION_H
#define __MT7925_MLO_COMPLETION_H

#include <linux/spinlock.h>
#include <net/cfg80211.h>

struct ieee80211_vif;

enum mt792x_mlo_core_phase {
	MT792X_MLO_CORE_IDLE,
	MT792X_MLO_CORE_QUEUED,
	MT792X_MLO_CORE_RUNNING,
	MT792X_MLO_CORE_DONE,
};

struct mt792x_mlo_ticket {
	u64 cookie;
	u64 assoc_epoch;
	u16 target;
	u16 expected_active;
};

struct mt792x_mlo_result {
	struct mt792x_mlo_ticket ticket;
	enum mt792x_mlo_core_phase phase;
	int core_ret;
	/* Observed callback failure; zero means UNKNOWN, not FW success. */
	int firmware_error;
	u16 final_active;
	u16 validated_links;
	unsigned long finished_jiffies;
};

struct mt792x_mlo_completion {
	spinlock_t lock;
	struct wiphy_work work;
	struct mt792x_mlo_result result;
	u64 next_cookie;
	u64 assoc_epoch;
	bool initialized;
	bool enabled;
	bool executing;
};

void mt7925_mlo_completion_init(struct ieee80211_vif *vif);
void mt7925_mlo_completion_assoc(struct ieee80211_vif *vif, bool assoc);
void mt7925_mlo_completion_stop(struct ieee80211_vif *vif);
void mt7925_mlo_completion_invalidate(struct ieee80211_vif *vif);
void mt7925_mlo_completion_reset_iter(void *data, u8 *mac,
				     struct ieee80211_vif *vif);
void mt7925_mlo_completion_changed(struct ieee80211_vif *vif);
void mt7925_mlo_completion_key_changed(struct ieee80211_vif *vif);
void mt7925_mlo_completion_supersede(struct ieee80211_vif *vif);
void mt7925_mlo_completion_note_error(struct ieee80211_vif *vif, int error);
int mt7925_mlo_completion_request(struct ieee80211_vif *vif, u16 target,
				struct mt792x_mlo_ticket *ticket);
int mt7925_mlo_completion_request_after(struct ieee80211_vif *vif, u16 target,
				      const struct mt792x_mlo_ticket *predecessor,
				      struct mt792x_mlo_ticket *ticket);
/* Returns lookup/lifecycle status. On zero (DONE), inspect result->core_ret;
 * do not conflate a setter's -EINPROGRESS/-ESTALE/-ECANCELED with our status.
 */
int mt7925_mlo_completion_result(struct ieee80211_vif *vif,
			       const struct mt792x_mlo_ticket *ticket,
			       struct mt792x_mlo_result *result);
bool mt7925_mlo_completion_validate(struct ieee80211_vif *vif,
				    const struct mt792x_mlo_ticket *ticket,
				    u16 authenticated_links);

#endif
