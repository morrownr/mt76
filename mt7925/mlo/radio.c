// SPDX-License-Identifier: ISC
/* Single owner of MLO radio requests, real grant reconciliation and release.
 * phy->mlo_roc remains the only reservation state; MCU code only serializes
 * requests and decodes firmware events. No host request certifies a grant.
 */
#include <linux/moduleparam.h>
#include "../mt7925.h"
#include "../mcu.h"
#include "../mlo_roc_state.h"
#include "../mlo_roc_delta.h"
#include "../mlo_pair_transition.h"
#include "../mlo_reqtype_test.h"
#include "mlo.h"
#include "radio.h"

/* TEST-ONLY: does not affect capability advertisement or association ROC. */
static int mt7925_mlo_test_reqtype = -1;

static int
mt7925_mlo_test_reqtype_set(const char *val, const struct kernel_param *kp)
{
	int override, ret;

	ret = kstrtoint(val, 0, &override);
	if (ret)
		return ret;
	if (!mt7925_mlo_test_reqtype_valid(override))
		return -EINVAL;
	return param_set_int(val, kp);
}

static const struct kernel_param_ops mt7925_mlo_test_reqtype_ops = {
	.set = mt7925_mlo_test_reqtype_set,
	.get = param_get_int,
};
module_param_cb(mlo_test_reqtype, &mt7925_mlo_test_reqtype_ops,
		&mt7925_mlo_test_reqtype, 0644);
MODULE_PARM_DESC(mlo_test_reqtype,
		"TEST-ONLY MT7927 delta ACQUIRE reqtype: -1=AUTO (default), 0=JOIN, 10=MLSR_AG, 11=MLSR_AA");

/* TEST-ONLY (default N): also apply mlo_test_reqtype to the association-time JOIN. The vendor stack selects MLSR
 * request types (10 AG, 11 AA) for single-radio MLO connections; this lets the 5+6 GHz (AA) pair be requested that way
 * to observe what the firmware grants. Never changes behaviour unless both parameters are set.
 */
static bool mt7925_mlo_test_reqtype_initial;
module_param_named(mlo_test_reqtype_initial, mt7925_mlo_test_reqtype_initial, bool, 0644);
MODULE_PARM_DESC(mlo_test_reqtype_initial,
		 "TEST-ONLY: apply mlo_test_reqtype to the association-time MT7927 JOIN too (default N)");

/* A/B diagnostic: dbdcband policy for the MT7927 non-2.4GHz JOIN pair.
 * 0 = current (ACQUIRE 0xfe, SUB_LINK = installed band_idx), 1 = Aug-30
 * positional (ordinal i: anchor 0, SUB_LINK 1; the only recorded real 5 GHz
 * airtime in 0x6), 2 = explicit installed band_idx for both entries.
 */
static int mt7925_mlo_roc_dbdc_policy;
module_param_named(mlo_roc_dbdc_policy, mt7925_mlo_roc_dbdc_policy, int, 0644);
MODULE_PARM_DESC(mlo_roc_dbdc_policy,
		"TEST-ONLY MT7927 5+6GHz JOIN dbdcband policy: 0=current, 1=positional, 2=installed band_idx for both, 3=0xff both, 4=0xfe both");

u8
mt7927_mlo_join_roc_dbdcband(struct mt792x_bss_conf *mconf,
			    struct ieee80211_channel *chan, u16 tag,
			    bool is_AG_band, u8 link_id,
			    u8 positional_band)
{
	u8 band_idx = mconf->mt76.band_idx;
	u8 dbdcband = (is_AG_band || (tag == UNI_ROC_ACQUIRE &&
				      !READ_ONCE(mt7925_mlo_roc_dbdc_policy))) ?
		      0xfe : band_idx;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_ROC_CONTEXT_AB: link_id=%u freq=%u bss_idx=%u installed_band_idx=%u tag=%u ag_pair=%u old_positional_dbdcband=%u requested_dbdcband=%u\n",
		       link_id, chan->center_freq, mconf->mt76.idx, band_idx,
		       tag, is_AG_band, positional_band, dbdcband);

	if (is_AG_band)
		return 0xfe;

	switch (READ_ONCE(mt7925_mlo_roc_dbdc_policy)) {
	case 1:
		return positional_band;
	case 2:
		return mconf->mt76.band_idx;
	case 3:
		return 0xff;	/* EMLSR/BAND_AUTO without DBDC, both entries */
	case 4:
		return 0xfe;	/* STR/BAND_ALL with DBDC, both entries */
	default:
		break;
	}

	if (tag == UNI_ROC_ACQUIRE)
		return 0xfe;

	return mconf->mt76.band_idx;
}

u8 mt7927_mlo_radio_reqtype(struct mt792x_phy *phy, bool is_AG_band,
			   bool delta)
{
	struct wiphy *wiphy = phy->mt76->hw->wiphy;
	u8 type;

	if (!(wiphy->iftype_ext_capab[0].mld_capa_and_ops &
	      IEEE80211_MLD_CAP_OP_MAX_SIMUL_LINKS))
		type = is_AG_band ? MT7925_ROC_REQ_MLSR_AG :
				    MT7925_ROC_REQ_MLSR_AA;
	else
		type = MT7925_ROC_REQ_JOIN;

	return mt7925_mlo_test_reqtype_resolve(READ_ONCE(mt7925_mlo_test_reqtype),
					    delta || READ_ONCE(mt7925_mlo_test_reqtype_initial),
					    type);
}

int mt7927_mlo_radio_test_reqtype(void)
{
	return READ_ONCE(mt7925_mlo_test_reqtype);
}

u8 mt7927_mlo_radio_slot_dbdc(struct mt792x_phy *phy,
			     struct mt792x_bss_conf *mconf,
			     struct ieee80211_channel *chan, u8 type,
			     bool is_AG_band, u16 tag, u8 ordinal, bool delta)
{
	if (type != MT7925_ROC_REQ_JOIN)
		return 0xff;
	if (delta || is_mt7927(&phy->dev->mt76))
		return mt7927_mlo_join_roc_dbdcband(mconf, chan, tag,
						 is_AG_band, mconf->link_id, ordinal);
	return is_AG_band ? 0xfe : ordinal;
}

bool mt7927_mlo_link_ctx_installed(struct mt792x_dev *dev,
				   struct mt792x_vif *mvif, unsigned int link_id)
{
	struct mt792x_bss_conf *link = mt792x_vif_to_link(mvif, link_id);

	return !((link == &mvif->bss_conf && link_id != mvif->deflink_id) ||
		 !(dev->mt76.vif_mask & BIT_ULL(link->mt76.idx)) ||
		 link->mt76.band_idx == 0xff);
}

int mt7927_mlo_radio_plan(struct mt792x_phy *phy,
			   struct mt792x_bss_conf *mconf, u16 sel_links,
			   struct mt7927_mlo_radio_plan *plan)
{
	struct mt792x_vif *mvif;
	struct ieee80211_vif *vif;
	unsigned long remaining;
	unsigned int i, count = 2;
	bool is_AG_band = false;

	if (!mconf)
		return -EPERM;
	mvif = mconf->vif;
	vif = container_of((void *)mvif, struct ieee80211_vif, drv_priv);
	if (READ_ONCE(mt7925_test_mt7927_mlo_join3) &&
	    is_mt7927(&phy->dev->mt76) && hweight16(sel_links) == 3)
		count = 3;
	if (hweight16(vif->valid_links) < 2 || hweight16(sel_links) != count)
		return -EPERM;

	memset(plan, 0, sizeof(*plan));
	plan->count = count;
	plan->anchor_link = (BIT(mconf->link_id) & sel_links) ?
			    mconf->link_id : __ffs(sel_links);
	remaining = sel_links & ~BIT(plan->anchor_link);
	for (i = 0; i < count; i++) {
		struct ieee80211_bss_conf *conf;
		struct mt792x_bss_conf *link;
		u8 id = i ? __ffs(remaining) : plan->anchor_link;

		remaining &= ~BIT(id);
		conf = mt792x_vif_to_bss_conf(vif, id);
		if (WARN_ON_ONCE(!conf) || WARN_ON_ONCE(!conf->chanreq.oper.chan))
			return -EPERM;
		link = mt792x_vif_to_link(mvif, id);
		/* The common resolver's anchor fallback does not own another
		 * requested link's BSS. Require the installed per-link context.
		 */
		if (is_mt7927(&phy->dev->mt76) &&
		    !mt7927_mlo_link_ctx_installed(phy->dev, mvif, id)) {
			if (READ_ONCE(mt76_mlo_diag_trace) ||
			    READ_ONCE(mt7925_mlo_ctl_trace))
				pr_info("MLO_ROC_BUILD_REJECT: link_id=%u anchor=%u deflink=%u fallback_to_anchor=%d bss_idx=%u band_idx=%u vif_mask=0x%llx sel_links=0x%x\n",
					id, plan->anchor_link, mvif->deflink_id,
					link == &mvif->bss_conf, link->mt76.idx,
					link->mt76.band_idx, phy->dev->mt76.vif_mask,
					sel_links);
			return -ENXIO;
		}
		plan->link[i].id = id;
		plan->link[i].mconf = link;
		plan->link[i].chan = conf->chanreq.oper.chan;
		plan->link[i].tag = i ? UNI_ROC_SUB_LINK : UNI_ROC_ACQUIRE;
		is_AG_band |= conf->chanreq.oper.chan->band == NL80211_BAND_2GHZ;
	}
	plan->reqtype = mt7927_mlo_radio_reqtype(phy, is_AG_band, false);
	for (i = 0; i < count; i++)
		plan->link[i].dbdcband = mt7927_mlo_radio_slot_dbdc(phy,
			plan->link[i].mconf, plan->link[i].chan, plan->reqtype,
			is_AG_band, plan->link[i].tag, i, false);
	return 0;
}

/* An MLO JOIN reserves one firmware slot per affiliated link (the
 * UNI_ROC_ACQUIRE anchor plus one UNI_ROC_SUB_LINK per additional link).
 * Both call sites that release a stale JOIN before requesting a new one
 * (mt7925_set_mlo_roc()'s own stale-token check and
 * mt7925_change_vif_links()'s teardown) used to send a single abort for
 * only the caller's mconf (the deflink/anchor), which releases just the
 * ACQUIRE slot. Any SUB_LINK slot for a link that survives into the next
 * pair (e.g. link1 reused across a 0x6->0x3 transition) was never
 * released, so firmware kept that link's bss_idx permanently reserved and
 * silently refused every later JOIN that touched it -- proven live via
 * ROC_ABORT_DIAG: token 11's grant for sel_links=0x6 (anchor=link2
 * bss_idx=0, sub_link=link1 bss_idx=1) only ever had bss_idx=0 aborted;
 * the following sel_links=0x3 JOIN (token 13, reusing link1/bss_idx=1)
 * timed out with no grant. Release every link actually held by the
 * reservation, mirroring mt7925_roc_iter()'s loop above.
 */
static void mt7925_mlo_roc_trace(struct mt792x_phy *phy, const char *reason)
{
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_ROC_STATE: reason=%s held_token=%u held_links=0x%x pending_token=%u pending_links=0x%x\n",
		       reason, phy->mlo_roc.held_token, phy->mlo_roc.held_links,
		       phy->mlo_roc.pending_token, phy->mlo_roc.pending_links);
}

/* Continuity invariant (mlo_roc_state.h): never release the last
 * security-valid grant. Every ROC abort that can hit a held MLO slot asks
 * here first. Allowed: association teardown, a slot that is not the
 * security-valid continuity grant (removed link, a request's own token, an
 * orphan, an obsolete handed-off incarnation), a continuity grant whose
 * role another security-valid incarnation already carries, or an explicit
 * destructive test gate. Anything else is refused and logged; the caller
 * leaves the slot held.
 */
static bool __mt7925_mlo_roc_abort_guard(struct mt792x_phy *phy,
					 struct mt792x_vif *mvif,
					 unsigned int token, unsigned int link_id,
					 unsigned int bss_idx, bool teardown,
					 const char *site)
{
	struct ieee80211_vif *vif;
	int v;

	if (!mvif)
		return true;

	vif = container_of((void *)mvif, struct ieee80211_vif, drv_priv);
	v = mt7925_mlo_roc_abort_check(&phy->mlo_roc, token, bss_idx,
				       vif->cfg.assoc && !teardown,
				       mt7927_mlo_destructive_test_gate());
	if (mt7925_mlo_roc_abort_verdict_allows(v)) {
		if (v != MT7925_MLO_ABORT_ALLOW_UNTRACKED &&
		    v != MT7925_MLO_ABORT_ALLOW_NON_CONTINUITY &&
		    (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace)))
			printk(KERN_INFO "MLO_CONTINUITY_ABORT_ALLOWED: token=%u link_id=%u bss_idx=%u active_links=0x%x valid_links=0x%x reason=%s site=%s\n",
			       token, link_id, bss_idx, vif->active_links,
			       vif->valid_links,
			       mt7925_mlo_roc_abort_verdict_name(v), site);
		mt7925_mlo_grant_released(&phy->mlo_roc, token, bss_idx);
		return true;
	}

	phy->mlo_roc.abort_blocked++;
	printk(KERN_INFO "MLO_CONTINUITY_ABORT_BLOCKED: token=%u link_id=%u bss_idx=%u active_links=0x%x valid_links=0x%x reason=%s site=%s\n",
	       token, link_id, bss_idx, vif->active_links, vif->valid_links,
	       mt7925_mlo_roc_abort_verdict_name(v), site);
	return false;
}

bool mt7925_mlo_roc_abort_guard(struct mt792x_phy *phy,
				struct mt792x_vif *mvif, unsigned int token,
				unsigned int link_id, unsigned int bss_idx,
				const char *site)
{
	return __mt7925_mlo_roc_abort_guard(phy, mvif, token, link_id, bss_idx,
					    false, site);
}
EXPORT_SYMBOL_GPL(mt7925_mlo_roc_abort_guard);

/* Filter a (token, links) abort through the guard. */
static unsigned long mt7925_mlo_roc_guard_links(struct mt792x_phy *phy,
						struct mt792x_vif *mvif,
						unsigned int token,
						unsigned long mask, bool teardown,
						const char *site)
{
	unsigned int link_id;

	for_each_set_bit(link_id, &mask, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *mconf = mt792x_vif_to_link(mvif, link_id);

		if (mconf && !__mt7925_mlo_roc_abort_guard(phy, mvif, token,
							   link_id,
							   mconf->mt76.idx,
							   teardown, site))
			__clear_bit(link_id, &mask);
	}
	return mask;
}

/* Send one UNI_ROC_ABORT per link in @mask under @token_id. Sends only; the
 * caller owns the held/pending bookkeeping and has passed the slots through
 * the continuity guard. Return slots whose MCU command succeeded; this is
 * not an independent firmware radio-release readback.
 */
static unsigned long mt7925_abort_mlo_roc_slots(struct mt792x_phy *phy,
				       struct mt792x_vif *mvif, u8 token_id,
				       unsigned long mask)
{
	struct ieee80211_vif *vif =
		container_of((void *)mvif, struct ieee80211_vif, drv_priv);
	unsigned long submitted = 0;
	unsigned int link_id;

	for_each_set_bit(link_id, &mask, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *mconf = mt792x_vif_to_link(mvif, link_id);
		int ret;

		if (!mconf) {
			if (is_mt7927(&phy->dev->mt76))
				mt7925_mlo_completion_note_error(vif, -ENXIO);
			continue;
		}

		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "ROC_ABORT: site=abort_mlo_roc_all link_id=%u bss_idx=%u token=%u dbdcband=0xfe\n",
			       link_id, mconf->mt76.idx, token_id);

		ret = mt7925_mcu_abort_mlo_roc(phy, mconf, token_id);
		if (!ret)
			submitted |= BIT(link_id);
		else if (is_mt7927(&phy->dev->mt76))
			mt7925_mlo_completion_note_error(vif, ret);
	}
	return submitted;
}

/* Release held slots in @mask under the token that owns them. A slot the
 * continuity guard refuses stays held (and in the host bookkeeping).
 */
static void mt7925_mlo_roc_release_held(struct mt792x_phy *phy,
					struct mt792x_vif *mvif,
					unsigned long mask, bool teardown,
					const char *reason)
{
	u8 tokens[IEEE80211_MLD_MAX_NUM_LINKS] = {};
	unsigned int link_id;
	unsigned long rel;

	/* A delta acquisition owns its slot under its own token, so read
	 * each slot's owner before the state release forgets it.
	 */
	for (link_id = 0; link_id < ARRAY_SIZE(tokens); link_id++)
		tokens[link_id] = mt7925_mlo_roc_state_owner_token(&phy->mlo_roc,
								   link_id);
	mask &= phy->mlo_roc.held_links;
	for_each_set_bit(link_id, &mask, ARRAY_SIZE(tokens)) {
		struct mt792x_bss_conf *mconf = mt792x_vif_to_link(mvif, link_id);

		if (mconf && !__mt7925_mlo_roc_abort_guard(phy, mvif,
							   tokens[link_id],
							   link_id,
							   mconf->mt76.idx,
							   teardown, reason))
			__clear_bit(link_id, &mask);
	}
	if (is_mt7927(&phy->dev->mt76)) {
		/* A failed command cannot retire the only record of its owner.
		 * Success here is the existing MCU command result, not proof
		 * that firmware has physically released the radio.
		 */
		rel = 0;
		for_each_set_bit(link_id, &mask, ARRAY_SIZE(tokens))
			rel |= mt7925_abort_mlo_roc_slots(phy, mvif,
						       tokens[link_id], BIT(link_id));
		mt7925_mlo_roc_state_release(&phy->mlo_roc, rel);
	} else {
		/* Preserve the existing non-MT7927 MLO release behavior. */
		rel = mt7925_mlo_roc_state_release(&phy->mlo_roc, mask);
		for_each_set_bit(link_id, &rel, ARRAY_SIZE(tokens))
			mt7925_abort_mlo_roc_slots(phy, mvif, tokens[link_id],
						   BIT(link_id));
	}
	mt7925_mlo_roc_trace(phy, reason);
}

/* Release slots firmware granted late for a request that had already timed
 * out (an orphan: owned by no live request), under the token they were
 * granted with.
 */
static void mt7925_mlo_roc_release_orphans(struct mt792x_phy *phy,
					   struct mt792x_vif *mvif)
{
	unsigned long links = 0;
	unsigned int token, bss, link_id;

	bss = mt7925_mlo_roc_state_orphan_take(&phy->mlo_roc, &token);
	if (!bss)
		return;

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		struct mt792x_bss_conf *mconf = mt792x_vif_to_link(mvif, link_id);

		if (mconf && (bss & BIT(mconf->mt76.idx)))
			__set_bit(link_id, &links);
	}

	/* keyed by (token, bss_idx): an orphan on the survivor's bss under a
	 * cancelled token never matches the continuity grant's own token
	 */
	links = mt7925_mlo_roc_guard_links(phy, mvif, token, links, false,
					   "orphan_release");
	mt7925_abort_mlo_roc_slots(phy, mvif, token, links);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_ROC_STATE: reason=orphan_release orphan_token=%u orphan_bss=0x%x links=0x%lx\n",
		       token, bss, links);
}

/* Release every held slot under its owning token (plain ROC coexistence,
 * association teardown), plus any late-granted orphan.
 */
void mt7925_abort_mlo_roc_all(struct mt792x_phy *phy,
			      struct mt792x_vif *mvif)
{
	mt7925_mlo_roc_release_held(phy, mvif, phy->mlo_roc.held_links, false,
				    "release_all");
	mt7925_mlo_roc_release_orphans(phy, mvif);
}

/* RELEASE_OLD_GRANT: release every grant incarnation the continuity handoff
 * made OBSOLETE, under its own token, through the continuity guard.
 */
void mt7925_mlo_roc_release_obsolete(struct mt792x_phy *phy,
				     struct mt792x_vif *mvif)
{
	struct mt7925_mlo_roc_state *st = &phy->mlo_roc;
	unsigned int i, link_id;

	for (i = 0; i < MT7925_MLO_GRANT_MAX; i++) {
		if (st->grant[i].life != MT7925_MLO_GRANT_OBSOLETE)
			continue;
		for (link_id = 0; link_id < 3; link_id++) {
			struct mt792x_bss_conf *mconf =
				mt792x_vif_to_link(mvif, link_id);

			if (!mconf || mconf->mt76.idx != st->grant[i].bss_idx ||
			    !(st->held_links & BIT(link_id)) ||
			    mt7925_mlo_roc_state_owner_token(st, link_id) !=
			    st->grant[i].token)
				continue;
			mt7925_mlo_roc_release_held(phy, mvif, BIT(link_id),
						    false, "release_obsolete");
		}
	}
}
EXPORT_SYMBOL_GPL(mt7925_mlo_roc_release_obsolete);

/* Hand the old JOIN reservation back for an old_links -> new_links active
 * change. Each link's slot is independently abortable, and the slot of a
 * link that stays active backs its live channel/security context: aborting
 * it turned the untouched survivor into cm=1/dec=0 RX ~25ms later (live
 * 0x6->0x5), while releasing
 * nothing left the stale token blocking the replacement JOIN (same run
 * with mlo_roc_release=1, ..._roc1_20260918_165806). So release only the
 * removed links' slots; the survivor's slot stays HELD under its old
 * token. bit 8 of mlo_roc_release restores the old release-everything
 * behaviour for A/B testing.
 */
void mt7925_mlo_roc_release_transition(struct mt792x_phy *phy,
				       struct mt792x_vif *mvif,
				       u16 old_links, u16 new_links,
				       u16 valid_links)
{
	struct mt7925_mlo_roc_delta d;
	u16 held = phy->mlo_roc.held_links;
	u16 mask;
	u8 old_token = phy->mlo_roc.held_token;

	mt7925_mlo_roc_delta(old_links, new_links, valid_links, &d);
	if (mt7925_mlo_roc_release & 8)
		mask = held;
	else
		mask = mt7925_mlo_roc_release_mask(old_links, new_links,
						   valid_links, held);

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_PAIR_ROC_TRANSITION: stage=release old=0x%x new=0x%x removed=0x%x survivor=0x%x added=0x%x old_token=%u held=0x%x release=0x%x keep=0x%x\n",
		       old_links, new_links, d.removed, d.survivor, d.added,
		       old_token, held, mask, held & ~mask);

	/* new_links == 0 is the association teardown: everything goes */
	mt7925_mlo_roc_release_held(phy, mvif, mask, !new_links,
				    "selective_release");
	mt7925_mlo_roc_release_orphans(phy, mvif);
	clear_bit(MT76_STATE_ROC, &phy->mt76->state);
}

int mt7925_abort_roc_token(struct mt792x_phy *phy,
				  struct mt792x_bss_conf *mconf,
				  u8 token_id, bool mlo_join)
{
	if (!mt7925_mlo_roc_abort_guard(phy, mconf->vif, token_id,
					mconf->link_id, mconf->mt76.idx,
					"abort_roc_token"))
		return 0;

	if (mlo_join || mconf != &mconf->vif->bss_conf)
		return mt7925_mcu_abort_mlo_roc(phy, mconf, token_id);

	return mt7925_mcu_abort_roc(phy, mconf, token_id);
}

/* Standalone-SUB_LINK-ROC experiment: single-link ROC wrapper mirroring
 * mt7925_set_roc()'s exact safety pattern (test_and_set_bit busy-gate,
 * wait_event_timeout, clean abort+clear on every exit path), issuing
 * MT7925_ROC_REQ_SUB_LINK instead of a full 2-link JOIN, with a short 1s
 * bound so an ungranted request can't hold up the caller.
 *
 * Waits on the dedicated phy->sublink_roc_done predicate (mcu.c's
 * mt7925_mcu_roc_handle_grant_sub_link()), never the full-JOIN path's
 * phy->roc_grant -- those two completion signals are independent by
 * construction, so neither can accidentally satisfy the other's wait.
 * Like the full-JOIN path, this always aborts/releases the operation after
 * it completes (success or not) -- it is not a persistent reservation;
 * ongoing link association after a grant is a separate, later concern
 * handled by the caller's own admission-generation gating.
 */
int mt7925_set_sublink_roc(struct mt792x_phy *phy, struct mt792x_bss_conf *mconf,
			   struct ieee80211_channel *chan, u8 dbdcband)
{
	u8 token;
	int err;

	if (test_and_set_bit(MT76_STATE_ROC, &phy->mt76->state))
		return -EBUSY;

	phy->sublink_roc_done = false;
	phy->sublink_roc_status = 0;
	token = ++phy->roc_token_id;
	phy->sublink_roc_token_id = token;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		      "SUBLINK_ROC_REQ: token=%u link_id=%u bss_idx=%u band_idx=%u dbdcband=%u ch=%u\n",
		      token, mconf->link_id, mconf->mt76.idx,
		      mconf->mt76.band_idx, dbdcband, chan->hw_value);

	err = mt7925_mcu_set_sublink_roc(phy, mconf, chan, dbdcband, 1000, token);
	if (err < 0) {
		phy->sublink_roc_token_id = 0;
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return err;
	}

	if (!wait_event_timeout(phy->roc_wait, phy->sublink_roc_done, HZ)) {
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "SUBLINK_ROC_TIMEOUT: token=%u\n", token);
		phy->sublink_roc_token_id = 0;
		mt7925_abort_roc_token(phy, mconf, token, false);
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return -ETIMEDOUT;
	}

	/* A real event arrived and matched our token -- read firmware's own
	 * status rather than treating arrival itself as success.
	 */
	err = phy->sublink_roc_status ? -EACCES : 0;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "SUBLINK_ROC_RESULT: token=%u status=%u result=%d\n",
		      token, phy->sublink_roc_status, err);

	phy->sublink_roc_token_id = 0;
	mt7925_abort_roc_token(phy, mconf, token, false);
	clear_bit(MT76_STATE_ROC, &phy->mt76->state);
	return err;
}

/* TEST-ONLY (test_mt7927_mlo_roc_attach): attach the one added link's slot
 * to the held JOIN token. Only the ATTACH state is waited on; the held
 * reservation (the survivor's slot) is never touched or overwritten.
 */
static int mt7925_mlo_roc_attach(struct mt792x_phy *phy,
				 struct mt792x_bss_conf *anchor,
				 u16 sel_links)
{
	struct mt792x_vif *mvif = anchor->vif;
	u16 added = sel_links & ~phy->mlo_roc.held_links;
	u8 token = phy->mlo_roc.held_token;
	struct mt792x_bss_conf *aconf;
	unsigned int link_id = __ffs(added);
	int err;

	aconf = mt792x_vif_to_link(mvif, link_id);
	if (!aconf) {
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return -EINVAL;
	}

	phy->roc_attach_done = false;
	mt7925_mlo_roc_state_attach_request(&phy->mlo_roc, token, added,
					    BIT(aconf->mt76.idx));
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_ROC_STATE: reason=attach_request held_token=%u held_links=0x%x attach_token=%u attach_links=0x%x attach_bss=0x%x\n",
		       phy->mlo_roc.held_token, phy->mlo_roc.held_links,
		       phy->mlo_roc.attach_pending_token,
		       phy->mlo_roc.attach_pending_links,
		       phy->mlo_roc.attach_pending_bss);

	err = mt7925_mcu_set_mlo_attach_roc(phy, aconf, sel_links, 5, token);
	if (err < 0) {
		unsigned int tok;

		mt7925_mlo_roc_state_attach_timeout(&phy->mlo_roc, &tok);
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return err;
	}

	if (!wait_event_timeout(phy->roc_wait, phy->roc_attach_done, 4 * HZ)) {
		unsigned int tok;
		unsigned long cancel;

		dev_warn(phy->dev->mt76.dev,
			 "MLO ROC attach not granted (link 0x%x, held token %u)\n",
			 added, token);
		/* Cancel only the added slot; never the survivor's. */
		cancel = mt7925_mlo_roc_state_attach_timeout(&phy->mlo_roc, &tok);
		cancel = mt7925_mlo_roc_guard_links(phy, mvif, tok, cancel, false,
						    "attach_timeout");
		mt7925_abort_mlo_roc_slots(phy, mvif, tok, cancel);
		mt7925_mlo_roc_trace(phy, "attach_timeout");
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return -ETIMEDOUT;
	}

	mt7925_mlo_roc_trace(phy, "attach_grant");
	clear_bit(MT76_STATE_ROC, &phy->mt76->state);
	return 0;
}

/* TEST-ONLY (mlo_same_token_extend): reuse the continuity owner's HELD
 * token in a two-TLV JOIN -- TLV0 re-affirms the survivor's own slot
 * exactly as currently granted (mt7925_mcu_set_mlo_roc() derives this from
 * the survivor's live struct ieee80211_bss_conf, not from any recomputed
 * value), TLV1 requests the replacement. Unlike mt7925_mlo_roc_attach()
 * (one SUB_LINK TLV, added link only, already proven to get no firmware
 * event), this sends the SAME wire shape as a normal 2-link JOIN, differing
 * only in reusing the held token instead of a fresh one. Only the
 * ATTACH/extension state is waited on; held_links (the survivor) is never
 * touched or overwritten, and no ROC_ABORT is ever sent for the survivor's
 * (token, bss_idx) -- reusing the same attach_pending_* bookkeeping and
 * abort guard as mt7925_mlo_roc_attach() (keyed on (token, bss_idx), so
 * sharing a token with the continuity owner does not weaken the guard).
 */
static int mt7925_mlo_roc_same_token_extend(struct mt792x_phy *phy,
					    struct mt792x_bss_conf *survivor,
					    u16 sel_links)
{
	struct mt792x_vif *mvif = survivor->vif;
	struct ieee80211_vif *vif = container_of((void *)mvif,
						 struct ieee80211_vif, drv_priv);
	u16 added = sel_links & ~phy->mlo_roc.held_links;
	u8 token = phy->mlo_roc.held_token;
	struct mt792x_bss_conf *aconf;
	unsigned int link_id = __ffs(added);
	int err;

	aconf = mt792x_vif_to_link(mvif, link_id);
	if (!aconf) {
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return -EINVAL;
	}

	phy->roc_attach_done = false;
	mt7925_mlo_roc_state_attach_request(&phy->mlo_roc, token, added,
					    BIT(aconf->mt76.idx));
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_SAME_TOKEN_EXTEND_REQUEST: held_token=%u held_links=0x%x survivor_bss=%u replacement_link=%u replacement_bss=%u sel_links=0x%x\n",
		       token, phy->mlo_roc.held_links, survivor->mt76.idx,
		       link_id, aconf->mt76.idx, sel_links);

	/* Same 2-TLV builder as a normal JOIN (initial association, 0x4->0x6
	 * growth): TLV0 = ACQUIRE for whichever link in sel_links matches
	 * survivor->link_id (the survivor, so it is re-affirmed, not the
	 * replacement), TLV1 = SUB_LINK for the other. token_id is the HELD
	 * token, not a fresh one -- the one deliberate difference from the
	 * always-worked growth case.
	 */
	err = mt7925_mcu_set_mlo_roc(phy, survivor, sel_links, 5, token);
	if (err < 0) {
		unsigned int tok;

		mt7925_mlo_roc_state_attach_timeout(&phy->mlo_roc, &tok);
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return err;
	}

	if (!wait_event_timeout(phy->roc_wait, phy->roc_attach_done, 4 * HZ)) {
		unsigned int tok;
		unsigned long cancel;

		dev_warn(phy->dev->mt76.dev,
			 "MLO same-token extend not granted (link 0x%x, held token %u)\n",
			 added, token);
		/* Cancel only the added slot; the survivor's (token, bss_idx)
		 * is a different grant-table key even though the token
		 * number is identical, so mt7925_mlo_roc_guard_links() finds
		 * it untracked/non-continuity and allows the abort -- never
		 * the survivor's own entry, which stays continuity_owner and
		 * BLOCK_CONTINUITY throughout.
		 */
		cancel = mt7925_mlo_roc_state_attach_timeout(&phy->mlo_roc, &tok);
		cancel = mt7925_mlo_roc_guard_links(phy, mvif, tok, cancel, false,
						    "same_token_extend_timeout");
		mt7925_abort_mlo_roc_slots(phy, mvif, tok, cancel);
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "MLO_SAME_TOKEN_EXTEND_TIMEOUT_KEEP_OWNER: token=%u survivor_bss=%u replacement_bss=%u active_links=0x%x valid_links=0x%x\n",
			       token, survivor->mt76.idx, aconf->mt76.idx,
			       vif->active_links, vif->valid_links);
		mt7925_mlo_roc_trace(phy, "same_token_extend_timeout");
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return -ETIMEDOUT;
	}

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_SAME_TOKEN_EXTEND_GRANT: token=%u replacement_bss=%u held_links=0x%x\n",
		       token, aconf->mt76.idx, phy->mlo_roc.held_links);
	mt7925_mlo_grant_note(&phy->mlo_roc, token, link_id, aconf->mt76.idx,
			      0xfe, 0, MT7925_MLO_GRANT_HELD);
	mt7925_mlo_roc_trace(phy, "same_token_extend_grant");
	clear_bit(MT76_STATE_ROC, &phy->mt76->state);
	return 0;
}

int mt7925_set_mlo_roc(struct mt792x_phy *phy,
			      struct mt792x_bss_conf *mconf,
			      u16 sel_links)
{
	int err;

	if (test_and_set_bit(MT76_STATE_ROC, &phy->mt76->state))
		return -EBUSY;

	/* TEST-ONLY 3-slot hold (test_mt7927_mlo_join3, default N): the very
	 * first association-time JOIN (nothing held, two links requested) is
	 * widened to every valid link. The dormant third link needs a
	 * firmware BSS to be named in a TLV; prime it exactly as the existing
	 * dormant-BSS test does (no STA/keys/ROC, never active).
	 */
	if (READ_ONCE(mt7925_test_mt7927_mlo_join3) &&
	    is_mt7927(&phy->dev->mt76) && !phy->mlo_roc.held_links &&
	    hweight16(sel_links) == 2 &&
	    hweight16(container_of((void *)mconf->vif, struct ieee80211_vif,
				   drv_priv)->valid_links) == 3 &&
	    (sel_links & ~container_of((void *)mconf->vif, struct ieee80211_vif,
				       drv_priv)->valid_links) == 0) {
		struct ieee80211_vif *jvif = container_of((void *)mconf->vif,
							  struct ieee80211_vif,
							  drv_priv);
		unsigned long third = jvif->valid_links & ~sel_links;
		unsigned int tl = __ffs(third);
		int perr = mt7927_mlo_prime_dormant_link(phy->dev, jvif,
							 mconf->vif, tl);

		printk(KERN_INFO
		       "MLO_JOIN3: widen sel_links 0x%x -> 0x%x third_link=%u prime_ret=%d\n",
		       sel_links, jvif->valid_links, tl, perr);
		if (!perr)
			sel_links = jvif->valid_links;
	}

	/* The firmware keeps the MLO JOIN reservation alive until it is
	 * explicitly aborted with the token it was granted under. Drop a
	 * stale one from a previous association, otherwise the new JOIN
	 * request is silently never granted.
	 */
	if (phy->mlo_roc.held_links && (mt7925_mlo_roc_release & 2)) {
		unsigned int stale = phy->mlo_roc.held_links;

		if (!(mt7925_mlo_roc_release & 8))
			stale = mt7925_mlo_roc_stale_release_mask(stale, sel_links);

		mt7925_mlo_roc_release_held(phy, mconf->vif, stale, false,
					    "stale_release");
	}

	mt7925_mlo_roc_release_orphans(phy, mconf->vif);

	if (READ_ONCE(mt7925_test_mt7927_mlo_roc_attach) &&
	    is_mt7927(&phy->dev->mt76) && phy->mlo_roc.held_token &&
	    phy->mlo_roc.held_links &&
	    !(phy->mlo_roc.held_links & ~sel_links) &&
	    hweight16(sel_links & ~phy->mlo_roc.held_links) == 1)
		return mt7925_mlo_roc_attach(phy, mconf, sel_links);

	phy->roc_grant = false;
	mt7925_mlo_roc_state_request(&phy->mlo_roc, phy->roc_token_id + 1,
				     sel_links);
	mt7925_mlo_roc_trace(phy, "request");

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "ROC_REQ_START: site=set_mlo_roc token=%u sel_links=0x%x duration=5\n",
		       phy->roc_token_id + 1, sel_links);

	err = mt7925_mcu_set_mlo_roc(phy, mconf, sel_links, 5, ++phy->roc_token_id);
	if (err < 0) {
		phy->mlo_roc.pending_token = 0;
		phy->mlo_roc.pending_links = 0;
		mt7925_mlo_roc_trace(phy, "request_failed");
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		goto out;
	}

	if (!wait_event_timeout(phy->roc_wait, phy->roc_grant, 4 * HZ)) {
		unsigned int tok;
		unsigned long abort_mask;

		dev_warn(phy->dev->mt76.dev,
			 "multi-link ROC not granted (links 0x%x, token %d)\n",
			 sel_links, phy->roc_token_id);
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "ROC_TIMEOUT: site=set_mlo_roc token=%u sel_links=0x%x\n",
			       phy->roc_token_id, sel_links);
		/* Cancel the request under ITS OWN token: firmware queues a
		 * JOIN whose slots conflict with a held one and grants it
		 * later (observed live, token 4 granted 28 s after the
		 * timeout once the survivor's token-3 slot went away), so an
		 * abandoned request must be aborted for every slot it named.
		 * The held reservation (the survivor's token-3 slot) is a
		 * different token and is never touched here.
		 */
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "MLO_RADIO_GRANT: tag=none reason=timeout token=%u sel_links=0x%x held_token=%u held_links=0x%x pending_links=0x%x\n",
			       phy->roc_token_id, sel_links,
			       phy->mlo_roc.held_token, phy->mlo_roc.held_links,
			       phy->mlo_roc.pending_links);
		abort_mask = mt7925_mlo_roc_state_timeout(&phy->mlo_roc, &tok);
		abort_mask = mt7925_mlo_roc_guard_links(phy, mconf->vif, tok,
							abort_mask, false,
							"join_timeout");
		mt7925_abort_mlo_roc_slots(phy, mconf->vif, tok, abort_mask);
		mt7925_mlo_roc_trace(phy, "timeout");
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		err = -ETIMEDOUT;
	} else {
		unsigned long glinks = phy->mlo_roc.pending_links;
		unsigned int gtok = phy->mlo_roc.pending_token;
		unsigned int link_id;

		if (mt7925_mlo_roc_state_grant(&phy->mlo_roc))
			dev_warn(phy->dev->mt76.dev,
				 "MLO ROC grant while slots of another token are still held\n");
		/* one grant incarnation per (token, bss_idx) slot */
		for_each_set_bit(link_id, &glinks, IEEE80211_MLD_MAX_NUM_LINKS) {
			struct mt792x_bss_conf *gconf =
				mt792x_vif_to_link(mconf->vif, link_id);

			if (gconf)
				mt7925_mlo_grant_note(&phy->mlo_roc, gtok, link_id,
						      gconf->mt76.idx, 0xfe, 0,
						      MT7925_MLO_GRANT_HELD);
		}
		mt7925_mlo_roc_trace(phy, "grant");
	}

out:
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_PAIR_ROC_TRANSITION: stage=join new=0x%x new_token=%u result=%d\n",
		       sel_links, phy->roc_token_id, err);
	return err;
}

/* Delta acquisition of ONE added link's physical slot (production pair
 * transition, see mlo_pair_transition.h). The survivor's slot stays held
 * under its own token and is never named here: a request that names it is
 * queued by firmware behind the held slot and never granted. The request is
 * one ACQUIRE/JOIN TLV for the added link only, on a fresh token, carrying
 * the firmware-selected dbdcband sentinel (0xfe) like every proven working
 * JOIN. @exp_slot is the physical slot the grant is expected to report (an
 * observation, not a request field). A grant is accepted only when its
 * token, bss and granted dbdcband match that expectation; anything else
 * leaves the survivor untouched and fails safe.
 */
int mt7925_mlo_roc_delta_acquire(struct mt792x_phy *phy,
				 struct mt792x_bss_conf *sub_mconf,
				 u16 sel_links, unsigned int exp_slot,
				 unsigned int gen)
{
	struct mt7925_mlo_roc_state *st = &phy->mlo_roc;
	struct mt792x_vif *mvif = sub_mconf->vif;
	unsigned long t0 = jiffies;
	unsigned int link_id = sub_mconf->link_id;
	int ret, check;
	u8 token;

	if (test_and_set_bit(MT76_STATE_ROC, &phy->mt76->state))
		return -EBUSY;

	mt7925_mlo_roc_release_orphans(phy, mvif);

	/* TEST-ONLY (mlo_same_token_extend): before allocating a fresh
	 * token for the usual one-TLV delta ACQUIRE, try reusing the
	 * continuity owner's held token in a two-TLV JOIN instead. Only
	 * when eligible (a held token exists and sel_links is exactly that
	 * held pair plus this one added link) and only when the current
	 * link actually belongs to the continuity owner's known survivor.
	 */
	if (READ_ONCE(mt7925_mlo_same_token_extend) &&
	    is_mt7927(&phy->dev->mt76) &&
	    mt7925_mlo_same_token_extend_eligible(st, sel_links)) {
		int cont = mt7925_mlo_continuity_get(st);

		if (cont >= 0) {
			struct mt792x_bss_conf *survivor =
				mt792x_vif_to_link(mvif, st->grant[cont].link_id);

			if (survivor)
				return mt7925_mlo_roc_same_token_extend(phy,
									survivor,
									sel_links);
		}
	}

	token = ++phy->roc_token_id;
	st->single_status = st->single_dbdc = st->single_ch = 0;
	st->single_evt_ms = 0;
	st->single_ev_bss = 0xffff;
	st->single_bss = sub_mconf->mt76.idx;
	st->single_t0 = t0;
	st->single_token = token;
	st->single_state = 1;
	mt7925_mlo_roc_trace(phy, "delta_request");

	/* the replacement is a new incarnation, never the continuity grant */
	mt7925_mlo_grant_note(st, token, link_id, sub_mconf->mt76.idx, 0xfe, 0,
			      MT7925_MLO_GRANT_PREPARING);

	ret = mt7925_mcu_set_mlo_single_acquire(phy, sub_mconf, sel_links, 5,
						token, exp_slot);
	if (ret < 0) {
		st->single_token = 0;
		st->single_state = 0;
		if (mt7925_mlo_roc_abort_guard(phy, mvif, token, link_id,
					       sub_mconf->mt76.idx,
					       "delta_send_failed"))
			mt7925_mcu_abort_mlo_roc(phy, sub_mconf, token);
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return ret;
	}

	if (!wait_event_timeout(phy->roc_wait, READ_ONCE(st->single_state) >= 2,
				4 * HZ)) {
		/* Firmware may still grant a queued request later: remember
		 * the token so the late grant is released as an orphan, and
		 * cancel only this request's own slot, never the survivor's.
		 */
		st->timedout_token = token;
		st->single_token = 0;
		st->single_state = 0;
		if (mt7925_mlo_roc_abort_guard(phy, mvif, token, link_id,
					       sub_mconf->mt76.idx,
					       "delta_timeout"))
			mt7925_mcu_abort_mlo_roc(phy, sub_mconf, token);
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "MLO_RADIO_GRANT: tag=none reason=timeout token=%u sel_links=0x%x held_token=%u held_links=0x%x delta=1\n",
			       token, sel_links, st->held_token, st->held_links);
		mt7925_mlo_roc_trace(phy, "delta_timeout");
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return -ETIMEDOUT;
	}

	check = mt7925_mlo_pt_grant_check(token, sub_mconf->mt76.idx, exp_slot,
					  token, st->single_ev_bss,
					  st->single_dbdc, st->single_status);
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO "MLO_PAIR_TRANSITION_GRANT: gen=%u token=%u link=%u bss=%u exp_slot=%u ev_bss=%u ev_dbdcband=%u ev_status=%u ev_ch=%u check=%d dt_ms=%u\n",
		       gen, token, link_id, sub_mconf->mt76.idx, exp_slot,
		       st->single_ev_bss, st->single_dbdc, st->single_status,
		       st->single_ch, check, st->single_evt_ms);

	st->single_token = 0;
	if (check != MT7925_MLO_PT_GRANT_OK) {
		st->single_state = 0;
		if (mt7925_mlo_roc_abort_guard(phy, mvif, token, link_id,
					       sub_mconf->mt76.idx,
					       "delta_grant_rejected"))
			mt7925_mcu_abort_mlo_roc(phy, sub_mconf, token);
		mt7925_mlo_roc_trace(phy, "delta_grant_rejected");
		clear_bit(MT76_STATE_ROC, &phy->mt76->state);
		return check == MT7925_MLO_PT_GRANT_FW_STATUS ? -EIO : -EPROTO;
	}

	st->single_state = 0;
	mt7925_mlo_roc_state_delta_grant(st, token, link_id);
	mt7925_mlo_grant_note(st, token, link_id, sub_mconf->mt76.idx,
			      st->single_dbdc, st->single_ch,
			      MT7925_MLO_GRANT_HELD);
	mt7925_mlo_roc_trace(phy, "delta_grant");
	clear_bit(MT76_STATE_ROC, &phy->mt76->state);
	return 0;
}

/* Survivor ownership handoff (mlo_survivor_rejoin experiment): release only
 * the survivor's held channel-manager slot, under the token that owns it,
 * through the same per-slot release the removed link already uses. The
 * caller re-JOINs the same survivor next; *held_before/after let it tell
 * whether firmware/host bookkeeping released anything broader.
 */
void mt7925_mlo_roc_survivor_release(struct mt792x_phy *phy,
				     struct mt792x_vif *mvif,
				     unsigned int survivor_link,
				     unsigned int *old_token,
				     unsigned int *held_before,
				     unsigned int *held_after)
{
	*held_before = phy->mlo_roc.held_links;
	*old_token = mt7925_mlo_roc_state_owner_token(&phy->mlo_roc,
						      survivor_link);
	/* destructive: only reachable behind the mlo_survivor_rejoin test
	 * gate, which the continuity guard recognises
	 */
	mt7925_mlo_roc_release_held(phy, mvif, BIT(survivor_link), false,
				    "survivor_handoff_release");
	*held_after = phy->mlo_roc.held_links;
}

static void mt7927_mlo_radio_reject(struct mt792x_phy *phy,
				   struct mt7925_roc_grant_tlv *grant,
				   const char *tag, const char *reason)
{
	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		pr_info("MLO_RADIO_EVENT_REJECT: tag=%s reason=%s token=%u bss=%u status=%u channel=%u pending_token=%u pending_links=0x%x held_token=%u held_links=0x%x\n",
			tag, reason, grant->tokenid, grant->bss_idx, grant->status,
			grant->primarychannel, phy->mlo_roc.pending_token,
			phy->mlo_roc.pending_links, phy->mlo_roc.held_token,
			phy->mlo_roc.held_links);
}

static bool mt7925_mlo_radio_single_event(struct mt792x_dev *dev,
					struct mt7925_roc_grant_tlv *grant,
					const char *tag)
{
	struct mt7925_mlo_roc_state *st = &dev->phy.mlo_roc;
	unsigned int dt;

	if (!st->single_token || grant->tokenid != st->single_token)
		return false;

	dt = jiffies_to_msecs(jiffies - st->single_t0);
	printk(KERN_INFO "MLO_ROC_SINGLE_EVENT: tag=%s token=%u bss=%u status=%u dbdcband=%u ch=%u band=%u itv=%u state_before=%u dt_ms=%u\n",
	       tag, grant->tokenid, grant->bss_idx, grant->status,
	       grant->dbdcband, grant->primarychannel, grant->rfband,
	       le32_to_cpu(grant->max_interval), st->single_state, dt);

	if (st->single_state == 1) {
		st->single_status = grant->status;
		st->single_ev_bss = grant->bss_idx;
		st->single_dbdc = grant->dbdcband;
		st->single_ch = grant->primarychannel;
		st->single_evt_ms = dt;
		st->single_state = grant->status ? 4 :
				   (!strcmp(tag, "grant") ? 2 : 3);
		wake_up(&dev->phy.roc_wait);
	}
	return true;
}

static bool mt7925_mlo_radio_attach_event(struct mt792x_dev *dev,
					struct mt7925_roc_grant_tlv *grant,
					const char *tag)
{
	struct mt792x_phy *phy = &dev->phy;

	/* As for the existing single-acquire and standalone SUB_LINK paths,
	 * a nonzero firmware status is not a grant. Keep the request pending
	 * for its existing bounded timeout; never promote the added slot.
	 */
	if (is_mt7927(&dev->mt76) && grant->status &&
	    grant->bss_idx < 32 &&
	    ((grant->tokenid == phy->mlo_roc.attach_pending_token &&
	      (phy->mlo_roc.attach_pending_bss & (1u << grant->bss_idx))) ||
	     (grant->tokenid == phy->mlo_roc.attach_timedout_token &&
	      (phy->mlo_roc.attach_timedout_bss & (1u << grant->bss_idx))))) {
		mt7927_mlo_radio_reject(phy, grant, tag, "attach_status");
		return true;
	}

	if (mt7925_mlo_roc_state_attach_grant(&phy->mlo_roc, grant->tokenid,
					      grant->bss_idx)) {
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "MLO_ROC_STATE: reason=attach_grant_event tag=%s token=%u bss=%u status=%u held_links=0x%x\n",
			       tag, grant->tokenid, grant->bss_idx,
			       grant->status, phy->mlo_roc.held_links);
		phy->roc_attach_done = true;
		wake_up(&phy->roc_wait);
		return true;
	}

	if (mt7925_mlo_roc_state_attach_late_grant(&phy->mlo_roc, grant->tokenid,
						   grant->bss_idx)) {
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO "MLO_ROC_STATE: reason=late_attach_grant tag=%s token=%u bss=%u orphan_bss=0x%x\n",
			       tag, grant->tokenid, grant->bss_idx,
			       phy->mlo_roc.orphan_bss);
		return true;
	}

	return false;
}

struct mt7927_mlo_radio_match {
	struct mt792x_phy *phy;
	struct mt7925_roc_grant_tlv *grant;
	bool matched;
};

static void mt7927_mlo_radio_match_iter(void *priv, u8 *mac,
				      struct ieee80211_vif *vif)
{
	struct mt7927_mlo_radio_match *match = priv;
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long links = match->phy->mlo_roc.pending_links;
	unsigned int id;

	if (vif->type != NL80211_IFTYPE_STATION || !ieee80211_vif_is_mld(vif) ||
	    mvif->phy != match->phy)
		return;
	for_each_set_bit(id, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *link = mt792x_vif_to_link(mvif, id);
		struct ieee80211_bss_conf *conf = mt792x_vif_to_bss_conf(vif, id);

		if (link && conf && conf->chanreq.oper.chan &&
		    link->mt76.idx == match->grant->bss_idx &&
		    conf->chanreq.oper.chan->hw_value == match->grant->primarychannel)
			match->matched = true;
	}
}

/* Called only with a fully decoded, bounds-checked actual firmware TLV.
 * Returning true consumes an MLO event; false leaves generic ROC handling
 * to the MCU adapter. The request type is not an ownership discriminator:
 * existing traces show firmware does not reliably echo it.
 */
bool mt7927_mlo_radio_event(struct mt792x_dev *dev,
			    struct mt7925_roc_grant_tlv *grant, bool sub_link)
{
	struct mt792x_phy *phy = &dev->phy;
	struct mt7925_mlo_roc_state *st = &phy->mlo_roc;
	const char *tag = sub_link ? "sub_link" : "grant";

	/* Existing reservation masks are keyed by BSS bits in unsigned int. */
	if (grant->bss_idx >= 32) {
		mt7927_mlo_radio_reject(phy, grant, tag, "bss_mask_range");
		return true;
	}

	if (mt7925_mlo_radio_single_event(dev, grant, tag) ||
	    mt7925_mlo_radio_attach_event(dev, grant, tag))
		return true;

	if (is_mt7927(&dev->mt76) && grant->status &&
	    grant->tokenid == st->timedout_token) {
		mt7927_mlo_radio_reject(phy, grant, tag, "late_status");
		return true;
	}
	if (mt7925_mlo_roc_state_late_grant(st, grant->tokenid, grant->bss_idx)) {
		mt7925_mlo_grant_note(st, grant->tokenid, 0xff, grant->bss_idx,
				      grant->dbdcband, grant->primarychannel,
				      MT7925_MLO_GRANT_ORPHAN);
		return true;
	}

	if (sub_link) {
		if (!phy->sublink_roc_token_id ||
		    grant->tokenid != phy->sublink_roc_token_id)
			return true;
		phy->sublink_roc_status = grant->status;
		phy->sublink_roc_done = true;
		wake_up(&phy->roc_wait);
		return true;
	}

	if (!st->pending_links)
		return false;
	if (is_mt7927(&dev->mt76)) {
		struct mt7927_mlo_radio_match match = {.phy = phy, .grant = grant};

		if (grant->tokenid != st->pending_token || grant->status) {
			mt7927_mlo_radio_reject(phy, grant, tag,
				grant->status ? "pending_status" : "pending_token");
			return true;
		}
		ieee80211_iterate_active_interfaces_atomic(dev->mt76.hw,
					IEEE80211_IFACE_ITER_RESUME_ALL,
					mt7927_mlo_radio_match_iter, &match);
		if (!match.matched) {
			mt7927_mlo_radio_reject(phy, grant, tag, "pending_bss_channel");
			return true;
		}
	}

	/* Only the real matching JOIN event can release the waiter. The
	 * transaction itself promotes pending slots after observing this.
	 * JOIN lifetime belongs to link teardown, never the generic ROC timer.
	 */
	phy->roc_grant = true;
	wake_up(&phy->roc_wait);
	clear_bit(MT76_STATE_ROC, &phy->mt76->state);
	return true;
}

bool mt7927_mlo_radio_expire(struct mt792x_phy *phy,
			     struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long links = phy->mlo_roc.pending_links;
	unsigned int id;

	if (!links || !(mt7925_mlo_roc_release & 1))
		return false;
	for_each_set_bit(id, &links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *conf = mt792x_vif_to_link(mvif, id);

		if (conf && (READ_ONCE(mt76_mlo_diag_trace) ||
			     READ_ONCE(mt7925_mlo_ctl_trace)))
			pr_info("ROC_ABORT: site=roc_iter_mlo_link link_id=%u token=%u dbdcband=0xfe\n",
				id, phy->roc_token_id);
		if (conf && mt7925_mlo_roc_abort_guard(phy, mvif, phy->roc_token_id,
						     id, conf->mt76.idx,
						     "roc_iter_mlo_link"))
			mt7925_mcu_abort_mlo_roc(phy, conf, phy->roc_token_id);
	}
	phy->mlo_roc.pending_links = 0;
	return true;
}

int mt7927_mlo_radio_begin_offchannel(struct mt792x_phy *phy,
				      struct mt792x_bss_conf *mconf)
{
	if (phy->mlo_roc.held_links && (mt7925_mlo_roc_release & 2))
		mt7925_abort_mlo_roc_all(phy, mconf->vif);
	if (test_and_set_bit(MT76_STATE_ROC, &phy->mt76->state))
		return -EBUSY;
	phy->roc_grant = false;
	phy->mlo_roc.pending_links = 0;
	return 0;
}

void mt7927_mlo_radio_beacon_loss(struct ieee80211_vif *vif, u8 bss_idx)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	unsigned long valid_links = vif->valid_links;
	unsigned int link_id;
	bool matched = false;

	/* MLO: a firmware beacon-loss event names a single BSS/link, not
	 * the whole MLD. mac80211 already distinguishes association
	 * membership (vif->valid_links) from current traffic activity
	 * (vif->active_links) -- losing one link's beacon health must not
	 * disconnect the whole MLD while another active link is still
	 * healthy. Only escalate to a full ieee80211_connection_loss() when
	 * this was the sole active link.
	 */
	for_each_set_bit(link_id, &valid_links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct mt792x_bss_conf *mconf =
			mt792x_vif_to_link(mvif, link_id);

		if (mconf && mconf->mt76.idx == bss_idx) {
			matched = true;
			break;
		}
	}

	if (!matched)
		return;

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_LINK_BEACON_LOSS: link=%u active=0x%x valid=0x%x\n",
		       link_id, vif->active_links, vif->valid_links);

	/* A valid link that is not currently active (retired by a pair shrink,
	 * dormant, or a failed admission) has no beacon-health bearing on the
	 * traffic-carrying link. Hardware: after a failed 0x6->0x5 request the
	 * removed link 1 reported beacon loss ~10 s later and the old code
	 * disconnected the whole association although link 2, the only active
	 * link, was healthy (transitions/A_default/journal.txt, 35532.960).
	 */
	if (!(vif->active_links & BIT(link_id))) {
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			       "MLO_LINK_BEACON_LOSS_IGNORED_INACTIVE: link=%u active=0x%x\n",
			       link_id, vif->active_links);
		return;
	}

	if (hweight16(vif->active_links) > 1) {
		if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
			printk(KERN_INFO
			       "MLO_LINK_BEACON_LOSS_SUPPRESSED_GLOBAL_DISCONNECT: link=%u active=0x%x\n",
			       link_id, vif->active_links);
		return;
	}

	if (READ_ONCE(mt76_mlo_diag_trace) || READ_ONCE(mt7925_mlo_ctl_trace))
		printk(KERN_INFO
		       "MLO_GLOBAL_CONNECTION_LOSS: link=%u active=0x%x\n",
		       link_id, vif->active_links);
	ieee80211_connection_loss(vif);
}
