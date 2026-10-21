/* Pure decision logic for the survivor-MLD-refresh A/B experiment
 * (test_mt7927_refresh_survivor_mld). See mt7927_mlo.c for the call site.
 *
 * STA_REC_MLD (mt7925_mcu_sta_mld_tlv(), mcu.c) serializes the peer's
 * current active-link partner table by iterating vif->active_links at
 * build time. For legs where a link's own band_idx does not change
 * (0x3<->0x5: neither the 2.4GHz link nor the 5/6GHz-slot link moves
 * physical radio), nothing in the existing transition path ever resends
 * that peer's STA_REC, so its firmware-held partner table stays whatever
 * it was the last time STA_REC was (re)built -- proven stale by
 * construction, not yet proven to matter to firmware. Legs touching 0x6
 * already force a full recreate+reauthorize on the survivor (band_idx
 * does change), which already resends STA_REC_MLD; this experiment is
 * redundant-but-harmless there and only materially new for 0x3<->0x5.
 */
#ifndef MT7925_MLO_SURVIVOR_REFRESH_H
#define MT7925_MLO_SURVIVOR_REFRESH_H

/* Links present in both the old and new active sets. Must return 0 (no
 * refresh) for initial admission (old_links==0) and complete teardown
 * (new_links==0) -- there is no "survivor" in either case.
 */
static inline unsigned long
mt7927_mlo_survivor_links(unsigned long old_links, unsigned long new_links)
{
	if (!old_links || !new_links)
		return 0;

	return old_links & new_links;
}

/* Refuse to serialize against anything but the FINAL published active set.
 * mac80211's own documented sequence calls change_sta_links twice per
 * switch: once with the transient union of old|new, once with the final
 * new_links. Serializing during the union call would encode a topology
 * that is about to be replaced again immediately -- worse than not
 * refreshing at all. cur_active_links is whatever vif->active_links reads
 * as at call time; intended_new_links is this invocation's own new_links
 * argument.
 */
static inline int
mt7927_mlo_survivor_refresh_ready(unsigned long cur_active_links,
				  unsigned long intended_new_links)
{
	return cur_active_links == intended_new_links;
}

#endif
