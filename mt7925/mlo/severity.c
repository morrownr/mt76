// SPDX-License-Identifier: BSD-3-Clause-Clear
/* MLO-owned severity memory for Policy V2.
 *
 * A link set that needed a firmware reset (mlo/recovery.c) is marked SEVERE by
 * band class and avoided for a fixed time, so steering carries on with the
 * remaining options instead of re-entering the same failure. The mark lives in
 * the private VIF, which outlives the reset, and is keyed by band because the AP
 * may return a different link set and numbering afterwards. The band map of the
 * first genuine three-band 0x7 is kept as a small record: it names the previous
 * connection when the live map no longer has all its bands. Only MLD
 * associations are ever classified; a non-MLO association is left alone.
 */
#include "../mt7925.h"
#include "../mlo_policy_modes.h"
#include "../mlo_severity.h"
#include "pairs.h"

static const char * const mt7925_mlo_sev_names[MT7925_MLO_SEV_NUM] = {
	"pair_2.4+5GHz", "pair_2.4+6GHz", "single_5GHz", "single_6GHz",
};

static struct mt7925_mlo_severity_state *mt7925_mlo_sev(struct ieee80211_vif *vif)
{
	return &((struct mt7925_mlo_vif *)vif->drv_priv)->severity;
}

static spinlock_t *mt7925_mlo_sev_lock(struct ieee80211_vif *vif)
{
	return &((struct mt7925_mlo_vif *)vif->drv_priv)->pair_lock;
}

static u64 mt7925_mlo_sev_now_ms(void)
{
	return div_u64(ktime_get_ns(), NSEC_PER_MSEC);
}

static bool mt7925_mlo_sev_supported(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;

	return mvif->phy && is_mt7927(&mvif->phy->dev->mt76) &&
	       vif->type == NL80211_IFTYPE_STATION;
}

static bool mt7925_mlo_sev_applies(struct ieee80211_vif *vif)
{
	return mt7925_mlo_sev_supported(vif) && ieee80211_vif_is_mld(vif);
}

/* At authorization: keep the band map of a genuine three-band 0x7. */
void mt7925_mlo_severity_lastgood_note(struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt7925_mlo_mode_ctx c;
	int bol[3];
	unsigned long flags;
	bool kept;

	if (!mt7925_mlo_sev_applies(vif))
		return;
	mt7925_mlo_build_mode_ctx(vif, mvif, &c);
	mt7925_mlo_mode_band_of_link(&c, bol);
	spin_lock_irqsave(mt7925_mlo_sev_lock(vif), flags);
	kept = mt7925_mlo_lastgood_capture(&mt7925_mlo_sev(vif)->lastgood,
					   vif->valid_links, bol, c.band_present,
					   READ_ONCE(mvif->mlo_completion.assoc_epoch));
	spin_unlock_irqrestore(mt7925_mlo_sev_lock(vif), flags);
	if (kept && (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
		printk(KERN_INFO "MLO_SEVERITY: last_good_0x7 recorded bands=L0:%d L1:%d L2:%d\n",
		       bol[0], bol[1], bol[2]);
}

/* First caller: the connection being left (the pair active before the failed
 * transition) is the previous one and is marked severe. The association may
 * already be gone (a lost association is one of the triggers), so this does not
 * require a live MLD. The live band map names the set when complete; the
 * recorded 0x7 map otherwise.
 */
void mt7925_mlo_severity_mark_previous(struct ieee80211_vif *vif, u16 prev_links)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt7925_mlo_mode_ctx c;
	int bol[3], cls;
	unsigned long long ttl;
	unsigned int strikes;
	unsigned long flags;

	if (!mt7925_mlo_sev_supported(vif))
		return;
	mt7925_mlo_build_mode_ctx(vif, mvif, &c);
	mt7925_mlo_mode_band_of_link(&c, bol);
	spin_lock_irqsave(mt7925_mlo_sev_lock(vif), flags);
	if (c.band_present == 0x7) {
		cls = mt7925_mlo_sev_class_of(prev_links, bol);
		if (cls < 0)
			cls = mt7925_mlo_lastgood_class_of(&mt7925_mlo_sev(vif)->lastgood,
							   prev_links);
	} else {
		cls = mt7925_mlo_lastgood_class_of(&mt7925_mlo_sev(vif)->lastgood,
						   prev_links);
		if (cls < 0)
			cls = mt7925_mlo_sev_class_of(prev_links, bol);
	}
	ttl = mt7925_mlo_sev_mark(&mt7925_mlo_sev(vif)->sev, cls, mt7925_mlo_sev_now_ms());
	strikes = cls >= 0 ? mt7925_mlo_sev(vif)->sev.strikes[cls] : 0;
	spin_unlock_irqrestore(mt7925_mlo_sev_lock(vif), flags);
	printk(KERN_INFO "MLO_SEVERITY: marked previous links=0x%x class=%s ttl_ms=%llu strikes=%u\n",
	       prev_links, cls >= 0 ? mt7925_mlo_sev_names[cls] : "none", ttl, strikes);
}

/* Second caller: decide which link set to run from the severity memory and the
 * links the new association actually offers (their bands, in their numbering).
 * Returns 0 when nothing is severe or no option is left, so the automatic
 * choice stands.
 */
u16 mt7925_mlo_severity_decide(struct ieee80211_vif *vif, unsigned long usable_links)
{
	int bol[3] = { MT7925_MLO_BAND_2G, MT7925_MLO_BAND_2G, MT7925_MLO_BAND_2G };
	unsigned long flags;
	unsigned int m;
	int link;

	if (!mt7925_mlo_sev_applies(vif))
		return 0;
	for_each_set_bit(link, &usable_links, 3) {
		int band = mt7925_mlo_band_index(mt7925_mlo_link_band(vif, link));

		if (band >= 0)
			bol[link] = band;
	}
	spin_lock_irqsave(mt7925_mlo_sev_lock(vif), flags);
	m = mt7925_mlo_sev_decide(&mt7925_mlo_sev(vif)->sev, bol, usable_links & 7,
				  mt7925_mlo_sev_now_ms());
	spin_unlock_irqrestore(mt7925_mlo_sev_lock(vif), flags);
	return m;
}

/* Steering: is this link set (in the live numbering) a severe class right now? */
bool mt7925_mlo_severity_blocked(struct ieee80211_vif *vif, u16 links,
				 const int band_of_link[3])
{
	unsigned long flags;
	bool blocked;
	int cls;

	if (!mt7925_mlo_sev_applies(vif))
		return false;
	cls = mt7925_mlo_sev_class_of(links, band_of_link);
	spin_lock_irqsave(mt7925_mlo_sev_lock(vif), flags);
	blocked = mt7925_mlo_sev_active(&mt7925_mlo_sev(vif)->sev, cls,
					mt7925_mlo_sev_now_ms());
	spin_unlock_irqrestore(mt7925_mlo_sev_lock(vif), flags);
	return blocked;
}

/* After the reset, once the new association is authorized: say what is still
 * avoided and what the record remembers, so the remaining options are explicit.
 */
void mt7925_mlo_severity_post_reset(struct ieee80211_vif *vif)
{
	struct mt7925_mlo_severity_state *st = mt7925_mlo_sev(vif);
	u64 now = mt7925_mlo_sev_now_ms();
	unsigned long flags;
	int cls;

	u16 decided;

	if (!mt7925_mlo_sev_applies(vif))
		return;
	decided = mt7925_mlo_severity_decide(vif, ieee80211_vif_usable_links(vif));
	if (decided)
		printk(KERN_INFO "MLO_SEVERITY: decision links=0x%x active=0x%x\n",
		       decided, vif->active_links);
	if (decided && decided == vif->active_links) {
		/* The decided pair is the one running: the transition completed. */
		spin_lock_irqsave(mt7925_mlo_sev_lock(vif), flags);
		mt7925_mlo_sev_settle(&st->sev);
		spin_unlock_irqrestore(mt7925_mlo_sev_lock(vif), flags);
		printk(KERN_INFO "MLO_SEVERITY: settled links=0x%x strikes cleared\n", decided);
	}
	for (cls = 0; cls < MT7925_MLO_SEV_NUM; cls++) {
		u64 until;

		spin_lock_irqsave(mt7925_mlo_sev_lock(vif), flags);
		until = st->sev.until_ms[cls];
		spin_unlock_irqrestore(mt7925_mlo_sev_lock(vif), flags);
		if (now < until)
			printk(KERN_INFO "MLO_SEVERITY: after_reset class=%s severe remaining_ms=%llu marks=%u record=%d\n",
			       mt7925_mlo_sev_names[cls], until - now, st->sev.marks[cls],
			       st->lastgood.valid);
	}
}

void mt7925_mlo_severity_show(struct seq_file *s, struct ieee80211_vif *vif)
{
	struct mt7925_mlo_severity_state *st = mt7925_mlo_sev(vif);
	u64 now = mt7925_mlo_sev_now_ms();
	unsigned long flags;
	int cls;

	spin_lock_irqsave(mt7925_mlo_sev_lock(vif), flags);
	seq_printf(s, "severity last_good_0x7=%u epoch=%llu bands=L0:%d,L1:%d,L2:%d\n",
		   st->lastgood.valid, st->lastgood.epoch, st->lastgood.band_of_link[0],
		   st->lastgood.band_of_link[1], st->lastgood.band_of_link[2]);
	for (cls = 0; cls < MT7925_MLO_SEV_NUM; cls++)
		seq_printf(s, "severity class=%s severe=%u remaining_ms=%llu marks=%u strikes=%u\n",
			   mt7925_mlo_sev_names[cls],
			   mt7925_mlo_sev_active(&st->sev, cls, now),
			   now < st->sev.until_ms[cls] ? st->sev.until_ms[cls] - now : 0,
			   st->sev.marks[cls], st->sev.strikes[cls]);
	spin_unlock_irqrestore(mt7925_mlo_sev_lock(vif), flags);
}
