#!/usr/bin/env python3
"""Execute shared MLO selectors/work against controlled device identities.

The break caught is allowing a shared-module experimental selector or sampler
to change MT7922 state. Production function bodies are compiled verbatim;
only kernel scheduling/locking/logging boundaries are replaced. This is an
offline host test, not firmware or live-device proof.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"\b" + re.escape(name) + r"\([^;{}]*?\)\n\{", source)
    start = source.rfind("\n", 0, match.start()) + 1
    if source[start:match.start()] == "":
        start = source.rfind("\n", 0, start - 1) + 1
    begin = source.index("{", match.start())
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#define S32_MIN INT32_MIN
#define U32_MAX UINT32_MAX
#define IEEE80211_MLD_MAX_NUM_LINKS 16
#define IEEE80211_LINK_UNSPECIFIED 15
#define IEEE80211_NUM_TIDS 16
#define IEEE80211_TTLM_NUM_TIDS 8
#define IEEE80211_TX_CTRL_PORT_CTRL_PROTO 1
#define IEEE80211_TX_CTRL_MLO_LINK 15
#define IEEE80211_TX_CTL_HW_80211_ENCAP 1
#define NL80211_IFTYPE_STATION 2
#define WLAN_CATEGORY_PROTECTED_EHT 37
#define ETH_ALEN 6
#define ETH_HLEN 14
#define MT_TXQ_PSD 4
#define IEEE80211_AC_BE 2
#define unlikely(x) (x)
#define u32_get_bits(v,m) ((v)&(m))
#define le16_to_cpu(x) (x)
#define ntohs(x) (x)
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define ether_addr_copy(a,b) memcpy(a,b,6)
#define pr_warn_ratelimited(...) fixture_log(__VA_ARGS__)
#define MT792x_WATCHDOG_TIME 250
#define MT_WCID_FLAG_PS 0
#define test_bit(n,p) ((*(p))&BIT(n))
#define mt76_chip(d) ((d)->chip)
#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
#define div64_u64(a,b) ((a)/(b))
#define hweight16(v) __builtin_popcount((unsigned)(v))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define rcu_dereference(p) (p)
#define rcu_dereference_protected(p,cond) (p)
#define lockdep_is_held(p) (true)
#define time_before(a,b) ((long)((a)-(b))<0)
#define time_after(a,b) time_before(b,a)
#define time_before_eq(a,b) ((long)((a)-(b))<=0)
#define msecs_to_jiffies(n) (n)
#define jiffies_to_msecs(n) (n)
#define jiffies_to_usecs(n) ((n)*1000)
#define for_each_set_bit(i,p,n) for ((i)=0; (i)<(n); (i)++) if (*(p)&BIT(i))
#define EXPORT_SYMBOL_GPL(x)
#define module_param_named(...)
#define MODULE_PARM_DESC(...)
#define KERN_INFO ""
static unsigned long jiffies = 1000;
static int mt76_mlo_diag_trace = 1;
static int logs;
static void fixture_log(const char *format, ...) { (void)format; logs++; }
#define printk(...) fixture_log(__VA_ARGS__)
typedef unsigned long atomic64_t;
typedef unsigned atomic_t;
#define ATOMIC_INIT(v) (v)
#define atomic64_read(p) (*(p))
#define atomic64_inc(p) ((*(p))++)
#define atomic_inc_return(p) (++*(p))
#define DEFINE_SPINLOCK(name) int name
#define spin_lock_bh(p) ((void)(p))
#define spin_unlock_bh(p) ((void)(p))
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct timer_list { int unused; };
#define timer_container_of(var,p,m) container_of(p,struct mt792x_phy,m)
struct mt792x_dev;
struct mt792x_sta;
struct mt792x_phy;
struct mt76_dev;
struct mt76_wcid;
struct ieee80211_vif;
struct ieee80211_sta;
struct sk_buff;
struct mt76_driver_ops {
    struct mt76_wcid *(*tx_select_wcid)(struct mt76_dev *, struct mt76_wcid *, struct ieee80211_sta *, struct sk_buff *);
    struct mt76_wcid *(*tx_select_link)(struct mt76_dev *, struct ieee80211_vif *, struct ieee80211_sta *, struct sk_buff *, struct mt76_wcid *);
    bool (*tx_preserve_addr3)(struct mt76_dev *, struct ieee80211_vif *, struct sk_buff *);
    void (*tx_update_load)(struct mt76_dev *);
};
struct mt76_wcid {
    bool sta, link_valid;
    unsigned link_id, idx, rssi, phy_idx;
    unsigned long ampdu_state, flags;
    struct { u32 tx_packets; } stats;
};
struct mt76_dev { unsigned chip; int mutex; struct mt76_wcid *wcid[4]; struct mt76_wcid global_wcid; const struct mt76_driver_ops *drv; };
struct mt76_phy {
    struct delayed_work mac_work;
    struct mt792x_phy *priv;
    struct mt76_dev *dev;
    unsigned mac_work_count;
    void *hw;
};
struct mt792x_phy {
    struct mt792x_dev *dev; struct mt76_phy *mt76;
    struct timer_list roc_timer; struct work_struct roc_work;
    unsigned roc_token_id;
    struct { unsigned pending_links; } mlo_roc;
};
struct mt792x_dev { struct mt76_dev mt76; int pm; };
struct mt792x_link_sta {
    struct mt76_wcid wcid;
    struct mt792x_sta *sta;
    u64 airtime_ac[4];
    u32 mlo_valid_rx_packets, tx_liveness_rx_seen, tx_liveness_tx_seen;
    unsigned long tx_liveness_rx_progress_jiffies;
    bool tx_liveness_rx_ever_seen;
};
struct mt792x_vif;
struct ieee80211_bss_conf { unsigned char addr[6], bssid[6]; };
struct ieee80211_link_sta { unsigned char addr[6]; };
struct mt792x_sta {
    struct mt792x_vif *vif;
    struct mt792x_link_sta deflink, *link[16];
    u8 deflink_id, mlo_tx_link_cursor, mlo_tid_link[16];
    bool mlo_tid_link_valid[16];
    unsigned long mlo_tid_link_ts[16], mlo_link_ready_jiffies[16];
};
struct mt792x_vif { struct mt792x_phy *phy; struct mt792x_sta sta; };
struct ieee80211_vif {
    bool mld; int type;
    unsigned long active_links;
    struct ieee80211_bss_conf *link_conf[16];
    struct { bool valid; unsigned long uplink[8]; } neg_ttlm;
    _Alignas(8) unsigned char drv_priv[sizeof(struct mt792x_vif)];
};
struct ieee80211_sta { unsigned char addr[6]; struct mt792x_sta *drv_priv; struct ieee80211_link_sta *link[16]; };
struct ieee80211_tx_info { unsigned flags; struct { unsigned flags; struct ieee80211_vif *vif; } control; };
struct sk_buff {
    struct ieee80211_tx_info info;
    unsigned priority, hash, len, protocol, qid;
    unsigned char *data;
    bool l4_hash, sw_hash;
};
#define IEEE80211_SKB_CB(s) (&(s)->info)
struct mt792x_bss_conf { struct { struct { struct { unsigned width; } def; } *ctx; } mt76; };
static struct mt792x_bss_conf *mt792x_vif_to_link(struct mt792x_vif *vif, unsigned id) {
    (void)vif; (void)id; return NULL;
}
#define cfg80211_chandef_get_width(d) ((d)->width)
static bool ieee80211_vif_is_mld(struct ieee80211_vif *vif) { return vif->mld; }
static bool is_mt7927(struct mt76_dev *dev) { return dev->chip == 0x7927; }
static unsigned ewma_signal_read(unsigned *rssi) { return *rssi; }
struct ieee80211_hdr { uint16_t frame_control, duration; unsigned char addr1[6],addr2[6],addr3[6]; uint16_t seq; };
struct ieee80211_hdr_3addr { unsigned char bytes[24]; };
struct ieee80211_mgmt { uint16_t frame_control, duration; unsigned char da[6],sa[6],bssid[6]; uint16_t seq; union { struct { unsigned char category,action_code; } action; } u; };
struct ieee80211_tx_control { struct ieee80211_sta *sta; };
struct ieee80211_hw { struct mt76_phy *priv; struct mt792x_dev *dev; };
struct { const char *comm; } fixture_current = {.comm="offline"};
#define current (&fixture_current)
static unsigned task_pid_nr(void *task) { (void)task; return 0; }
static struct mt792x_dev *mt792x_hw_dev(struct ieee80211_hw *hw) { return hw->dev; }
static bool ieee80211_is_data(uint16_t f) { return (f&12)==8; }
static bool ieee80211_is_action(uint16_t f) { return (f&0xfc)==0xd0; }
static bool ieee80211_has_tods(uint16_t f) { return f&0x100; }
static bool ieee80211_has_protected(uint16_t f) { return f&0x4000; }
static unsigned skb_get_queue_mapping(struct sk_buff *s) { return s->qid; }
static void skb_set_queue_mapping(struct sk_buff *s,unsigned q) { s->qid=q; }
static struct mt76_wcid *sent;
static unsigned freed, tx_calls, pm_unref, traces;
static bool pm_ready=true;
static unsigned writable_calls;
static int writable_error;
static int skb_ensure_writable(struct sk_buff *skb, unsigned length)
{ (void)skb; (void)length; writable_calls++; return writable_error; }
static void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *s) { (void)hw;(void)s;freed++; }
static bool mt76_connac_pm_ref(struct mt76_phy *p,int *pm) { (void)p;(void)pm;return pm_ready; }
static void mt76_connac_pm_unref(struct mt76_phy *p,int *pm) { (void)p;(void)pm;pm_unref++; }
static void mt76_tx(struct mt76_phy *p,struct ieee80211_sta *s,struct mt76_wcid *w,struct sk_buff *b) { (void)p;(void)s;(void)b;sent=w;tx_calls++; }
static void mt76_connac_pm_queue_skb(struct ieee80211_hw *hw,int *pm,struct mt76_wcid *w,struct sk_buff *s) { (void)hw;(void)pm;(void)s;sent=w;tx_calls++; }
struct mt76_mlo_pkt_sig { unsigned unused; };
struct mt76_mlo_tx_select_trace {
    unsigned hash,protocol,orig_wcid,sel_wcid,path,priority,txq_tid,qid,orig_link,orig_phy,sel_link,sel_phy,info_link;
    bool l4_hash,sw_hash,sta_present,vif_present;
    unsigned char da[6];
};
static void mt76_mlo_skb_pkt_sig(struct sk_buff *s,struct mt76_mlo_pkt_sig *sig) { (void)s;(void)sig; }
static void mt76_mlo_tx_select_set_pkt_sig(struct mt76_mlo_tx_select_trace *t,struct mt76_mlo_pkt_sig *sig) { (void)t;(void)sig; }
static void trace_mlo_tx_select(struct mt76_dev *dev,struct mt76_mlo_tx_select_trace *t) { (void)dev;(void)t;traces++; }
static int survey_updates, mib_updates, status_checks, work_queued, lock_depth;
static void mt792x_mutex_acquire(struct mt792x_dev *dev) { (void)dev; lock_depth++; }
static void mt792x_mutex_release(struct mt792x_dev *dev) { (void)dev; lock_depth--; }
static void mt76_update_survey(struct mt76_phy *phy) { (void)phy; survey_updates++; }
static void mt792x_mac_update_mib_stats(struct mt792x_phy *phy) { (void)phy; mib_updates++; }
static void mt76_tx_status_check(struct mt76_dev *dev, bool flush) { (void)dev; (void)flush; status_checks++; }
static void ieee80211_queue_delayed_work(void *hw, struct delayed_work *work, unsigned delay) {
    (void)hw; (void)work; (void)delay; work_queued++;
}
static void ieee80211_queue_work(void *hw, struct work_struct *work) {
    (void)hw; (void)work; work_queued++;
}
'''


MAIN = r'''
static struct mt792x_dev dev = {.mt76={.chip=0x7927}};
static struct mt76_phy mphy = {.dev=&dev.mt76};
static struct mt792x_phy phy = {.dev=&dev, .mt76=&mphy};
static struct ieee80211_vif vif = {.mld=true, .active_links=5};
static struct mt792x_sta msta;
static struct ieee80211_sta sta = {.drv_priv=&msta};
static struct mt792x_link_sta secondary;
static struct sk_buff skb;
static int fail(const char *name) { fprintf(stderr, "FAIL: %s\n", name); return 1; }
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    static const struct mt76_driver_ops owner_ops={
        .tx_select_wcid=mt7925_mlo_tx_select_wcid,
        .tx_select_link=mt7925_mlo_tx_select_link,
        .tx_preserve_addr3=mt7925_mlo_tx_preserve_addr3,
        .tx_update_load=mt7925_mlo_tx_update_load,
    };
    static const struct mt76_driver_ops ordinary_ops={0};
    dev.mt76.drv=&owner_ops;
    mphy.priv=&phy;
    msta.vif=(void *)vif.drv_priv;
    msta.vif->phy=&phy;
    msta.deflink.wcid=(struct mt76_wcid){.idx=1,.sta=true,.link_valid=true};
    msta.deflink.sta=&msta;
    msta.link[0]=&msta.deflink;
    secondary.wcid=(struct mt76_wcid){.idx=2,.link_id=2,.sta=true,.link_valid=true};
    secondary.sta=&msta;
    secondary.airtime_ac[0]=1234;
    msta.link[2]=&secondary;
    msta.mlo_link_ready_jiffies[2]=1;
    dev.mt76.wcid[0]=&secondary.wcid;
    mt7925_mlo_force_tx_link=2;
    struct mt76_wcid *base=&msta.deflink.wcid;
    if (!strcmp(argv[1], "bulk_mt7922") || !strcmp(argv[1], "bulk_mt7922_mld")) {
        dev.mt76.chip=0x7922;
        vif.mld=!strcmp(argv[1], "bulk_mt7922_mld");
        for (int mode=-5;mode<=2;mode++) {
            mt7925_mlo_force_tx_link=mode;
            for (unsigned port=0;port<2;port++) {
                skb.info.control.flags=port;
                if (mt7925_mlo_tx_select_wcid(&dev.mt76,base,&sta,&skb)) return fail("MT7922 WCID overridden");
                if (logs || msta.mlo_tid_link_valid[0] || msta.mlo_tx_link_cursor)
                    return fail("MT7922 experimental state/log changed");
            }
        }
    } else if (!strcmp(argv[1], "bulk_non_mld")) {
        vif.mld=false; skb.info.control.flags=1;
        if (mt7925_mlo_tx_select_wcid(&dev.mt76,base,&sta,&skb) || logs) return fail("non-MLD overridden");
    } else if (!strcmp(argv[1], "bulk_null_vif")) {
        msta.vif=NULL;
        if (mt7925_mlo_tx_select_wcid(&dev.mt76,base,&sta,&skb)) return fail("NULL vif overridden");
    } else if (!strcmp(argv[1], "bulk_null_sta")) {
        if (mt7925_mlo_tx_select_wcid(&dev.mt76,base,NULL,&skb)) return fail("NULL sta overridden");
    } else if (!strcmp(argv[1], "bulk_mt7927")) {
        if (mt7925_mlo_tx_select_wcid(&dev.mt76,base,&sta,&skb)!=&secondary.wcid) return fail("MT7927 forced link lost");
        skb.info.control.flags=1;
        if (mt7925_mlo_tx_select_wcid(&dev.mt76,&secondary.wcid,&sta,&skb)!=base) return fail("MT7927 control-port anchor changed");
        skb.info.control.flags=0; mt7925_mlo_force_tx_link=-1;
        /* The shipped default is the primary-WCID handle (single PN owner); no TID pin is created. */
        if (mt7925_mlo_tx_primary_handle!=1) return fail("MT7927 primary handle is not the default");
        if (mt7925_mlo_tx_select_wcid(&dev.mt76,base,&sta,&skb)!=base || msta.mlo_tid_link_valid[0])
            return fail("MT7927 default primary-WCID handle lost");
        /* The per-TID pin policy stays selectable and unchanged (handle 0). */
        mt7925_mlo_tx_primary_handle=0;
        if (mt7925_mlo_tx_select_wcid(&dev.mt76,base,&sta,&skb)!=&secondary.wcid || !msta.mlo_tid_link_valid[0])
            return fail("MT7927 per-TID policy lost");
        mt7925_mlo_tx_primary_handle=1;
    } else if (!strcmp(argv[1], "bulk_invalid_forced") ||
               !strcmp(argv[1], "bulk_invalid_tid") ||
               !strcmp(argv[1], "bulk_invalid_control_port")) {
        bool tid=!strcmp(argv[1],"bulk_invalid_tid");
        bool control_port=!strcmp(argv[1],"bulk_invalid_control_port");
        secondary.wcid.link_valid=false;
        if(tid) {
            mt7925_mlo_force_tx_link=-1; mt7925_mlo_tx_primary_handle=0;
            msta.mlo_tid_link_valid[0]=true;msta.mlo_tid_link[0]=2;
            msta.mlo_tid_link_ts[0]=jiffies;
        }
        if(control_port) {
            skb.info.control.flags=IEEE80211_TX_CTRL_PORT_CTRL_PROTO;
            msta.deflink.wcid.link_valid=false;
        }
        if(mt7925_mlo_tx_select_wcid(&dev.mt76,base,&sta,&skb))
            return fail("invalid selected WCID must never leave queued owner");
        if(tid&&msta.mlo_tid_link_valid[0])return fail("invalid selected TID pin survived");
    } else if (!strcmp(argv[1], "generic_mt7922_poisoned") || !strcmp(argv[1], "generic_other_poisoned")) {
        dev.mt76.chip=!strcmp(argv[1], "generic_mt7922_poisoned")?0x7922:0x7915;
        if (fixture_select(&dev.mt76,base,(void *)1,&skb)!=base) return fail("foreign device selector changed WCID");
    } else if (!strcmp(argv[1], "generic_mt7927")) {
        if (fixture_select(&dev.mt76,base,&sta,&skb)!=&secondary.wcid) return fail("MT7927 per-device queued selection lost");
        if (fixture_select(&dev.mt76,base,NULL,&skb)!=base) return fail("NULL generic sta fallback");
        secondary.wcid.flags=BIT(MT_WCID_FLAG_PS);
        if (fixture_select(&dev.mt76,base,&sta,&skb)!=base) return fail("PS WCID fallback lost");
    } else if (!strcmp(argv[1], "immediate_mt7922") || !strcmp(argv[1], "immediate_mt7927")) {
        bool dut=!strcmp(argv[1], "immediate_mt7927");
        dev.mt76.chip=dut?0x7927:0x7922;
        struct ieee80211_hw hw={.priv=&mphy,.dev=&dev};
        struct ieee80211_tx_control control={.sta=&sta};
        struct ieee80211_hdr hdr={.frame_control=0x108};
        skb.data=(void *)&hdr;skb.len=sizeof(hdr);
        skb.info.control.vif=&vif;skb.info.flags=1;
        dev.mt76.drv=dut?&owner_ops:&ordinary_ops;
        mt792x_tx(&hw,&control,&skb);
        if (sent!=(dut?&secondary.wcid:base) || tx_calls!=1 || pm_unref!=1 || freed)
            return fail("immediate WCID/PM ownership");
        if (traces!=(dut?1U:0U) || (!dut && logs))
            return fail("immediate per-device dispatch/diagnostic identity");
        pm_ready=false;skb.qid=5;mt792x_tx(&hw,&control,&skb);
        if (tx_calls!=2 || skb.qid!=2 || pm_unref!=1 || freed) return fail("immediate PM queue ownership");
    } else if (!strcmp(argv[1], "immediate_mt7925_sta_baseline") ||
               !strcmp(argv[1], "immediate_mt7925_bmc_baseline") ||
               !strcmp(argv[1], "immediate_mt7925_single") ||
               !strcmp(argv[1], "immediate_mt7927_sta_cursor") ||
               !strcmp(argv[1], "immediate_mt7927_bmc_cursor")) {
        bool dut=strstr(argv[1],"mt7927")!=NULL;
        bool bmc=strstr(argv[1],"bmc")!=NULL;
        bool single=strstr(argv[1],"single")!=NULL;
        struct mt792x_link_sta middle={.wcid={.idx=3,.link_id=1,.sta=true,.link_valid=true}};
        struct ieee80211_hw hw={.priv=&mphy,.dev=&dev};
        struct ieee80211_tx_control control={.sta=bmc?NULL:&sta};
        struct ieee80211_hdr hdr={.frame_control=0x108};
        struct mt792x_sta *peer=&msta;
        dev.mt76.chip=dut?0x7927:0x7925;
        vif.active_links=single?4:7;msta.link[1]=&middle;
        if(bmc) {
            peer=&((struct mt792x_vif *)vif.drv_priv)->sta;
            *peer=msta;
        }
        skb.data=(void *)&hdr;skb.len=sizeof(hdr);
        skb.info.control.vif=&vif;skb.info.flags=IEEE80211_TX_CTL_HW_80211_ENCAP;
        skb.info.control.flags=IEEE80211_LINK_UNSPECIFIED;
        mt7925_mlo_force_tx_link=-1;
        mt792x_tx(&hw,&control,&skb);
        /* Saved dirty baseline before/tracked.diff:11860-11865 already
         * restricted UNSPEC RR to MT7927. Preserve that existing work. */
        struct mt76_wcid *want=!dut&&bmc?&peer->deflink.wcid:base;
        unsigned int cursor=!dut||single?0:bmc?1:2;
        if(sent!=want||peer->mlo_tx_link_cursor!=cursor||tx_calls!=1||freed||pm_unref!=1)
            return fail("saved MT7925 deflink baseline or owner MT7927 lookup count changed");
    } else if (!strcmp(argv[1], "immediate_writable_failure") ||
               !strcmp(argv[1], "immediate_native_pm_queue")) {
        bool failure=!strcmp(argv[1], "immediate_writable_failure");
        vif.type=NL80211_IFTYPE_STATION;
        struct ieee80211_hw hw={.priv=&mphy,.dev=&dev};
        struct ieee80211_tx_control control={.sta=&sta};
        struct ieee80211_hdr hdr={.frame_control=0x108,.addr1={0x11},.addr2={0x22},.addr3={0x33}};
        const struct ieee80211_hdr original=hdr;
        struct ieee80211_bss_conf conf={.addr={0x41},.bssid={0x42}};
        struct ieee80211_link_sta peer={.addr={0x43}};
        vif.link_conf[2]=&conf;sta.link[2]=&peer;
        skb.data=(void *)&hdr;skb.len=sizeof(hdr);skb.info.control.vif=&vif;
        skb.info.flags=0;skb.info.control.flags=2;skb.qid=5;
        mt7925_mlo_force_tx_link=-1;
        writable_error=failure;pm_ready=false;
        mt792x_tx(&hw,&control,&skb);
        if (writable_calls!=1) return fail("native shared TX must ensure writable once");
        if (failure) {
            if (freed!=1 || tx_calls || pm_unref || memcmp(&hdr,&original,sizeof(hdr)))
                return fail("writable failure must drop before headers or PM submission");
        } else if (sent!=&secondary.wcid || tx_calls!=1 || pm_unref || freed ||
                   skb.qid!=IEEE80211_AC_BE || hdr.addr1[0]!=0x43 ||
                   hdr.addr2[0]!=0x41 || hdr.addr3[0]!=0x42) {
            return fail("generic native TX must retain header and PM queue ownership");
        }
    } else if (!strcmp(argv[1], "immediate_inactive_sta") ||
               !strcmp(argv[1], "immediate_invalid_sta") ||
               !strcmp(argv[1], "immediate_no_live_sta") ||
               !strcmp(argv[1], "immediate_inactive_bmc") ||
               !strcmp(argv[1], "immediate_invalid_bmc") ||
               !strcmp(argv[1], "immediate_no_live_bmc")) {
        bool bmc=strstr(argv[1],"bmc")!=NULL;
        bool inactive=strstr(argv[1],"inactive")!=NULL;
        bool no_live=strstr(argv[1],"no_live")!=NULL;
        struct ieee80211_hw hw={.priv=&mphy,.dev=&dev};
        struct ieee80211_tx_control control={.sta=bmc?NULL:&sta};
        struct ieee80211_hdr hdr={.frame_control=0x108};
        if(bmc)((struct mt792x_vif *)vif.drv_priv)->sta=msta;
        vif.active_links=inactive?1:no_live?4:5;
        secondary.wcid.link_valid=inactive;
        skb.data=(void *)&hdr;skb.len=sizeof(hdr);skb.info.control.vif=&vif;
        skb.info.flags=IEEE80211_TX_CTL_HW_80211_ENCAP;skb.info.control.flags=2;
        mt7925_mlo_force_tx_link=-1;
        mt792x_tx(&hw,&control,&skb);
        if(no_live) {
            if(freed!=1||tx_calls||pm_unref)return fail("no live affiliated WCID must drop before submission");
        } else if(sent!=base||freed||tx_calls!=1||pm_unref!=1)
            return fail("inactive or invalid immediate hint must use live active fallback");
    } else if (!strcmp(argv[1], "immediate_missing_conf") ||
               !strcmp(argv[1], "immediate_missing_peer")) {
        struct ieee80211_hw hw={.priv=&mphy,.dev=&dev};
        struct ieee80211_tx_control control={.sta=&sta};
        struct ieee80211_hdr hdr={.frame_control=0x108};
        const struct ieee80211_hdr original=hdr;
        struct ieee80211_bss_conf conf={0};struct ieee80211_link_sta link_peer={0};
        vif.type=NL80211_IFTYPE_STATION;
        vif.link_conf[2]=!strcmp(argv[1],"immediate_missing_conf")?NULL:&conf;
        sta.link[2]=!strcmp(argv[1],"immediate_missing_peer")?NULL:&link_peer;
        skb.data=(void *)&hdr;skb.len=sizeof(hdr);skb.info.control.vif=&vif;
        skb.info.flags=0;skb.info.control.flags=2;mt7925_mlo_force_tx_link=-1;
        mt792x_tx(&hw,&control,&skb);
        if(freed!=1||tx_calls||pm_unref||memcmp(&hdr,&original,sizeof(hdr)))
            return fail("missing native link metadata must drop before mutation/submission");
    } else if (!strcmp(argv[1], "immediate_missing_sta") ||
               !strcmp(argv[1], "immediate_missing_bmc") ||
               !strcmp(argv[1], "immediate_missing_foreign")) {
        bool bmc=!strcmp(argv[1], "immediate_missing_bmc");
        bool foreign=!strcmp(argv[1], "immediate_missing_foreign");
        struct ieee80211_hw hw={.priv=&mphy,.dev=&dev};
        struct ieee80211_tx_control control={.sta=bmc?NULL:&sta};
        struct ieee80211_hdr hdr={.frame_control=0x108};
        struct mt792x_link_sta bmc_secondary={.wcid={.idx=32,.link_id=2,.link_valid=true}};
        struct mt792x_sta *bmc_sta=&((struct mt792x_vif *)vif.drv_priv)->sta;
        bmc_sta->vif=(void *)vif.drv_priv;
        bmc_sta->deflink.wcid=(struct mt76_wcid){.idx=31,.link_valid=true};
        bmc_sta->link[0]=&bmc_sta->deflink;bmc_sta->link[2]=&bmc_secondary;
        vif.active_links=4; /* Only link2 is usable; explicit link1 is absent. */
        mt7925_mlo_force_tx_link=-1;
        dev.mt76.chip=foreign?0x7922:0x7927;
        dev.mt76.drv=foreign?&ordinary_ops:&owner_ops;
        skb.data=(void *)&hdr;skb.len=sizeof(hdr);
        skb.info.control.vif=&vif;skb.info.flags=IEEE80211_TX_CTL_HW_80211_ENCAP;
        skb.info.control.flags=1;
        mt792x_tx(&hw,&control,&skb);
        if (foreign) {
            if (freed!=1 || tx_calls || traces || pm_unref)
                return fail("foreign absent explicit link must retain ordinary drop");
        } else if (sent!=(bmc?&bmc_secondary.wcid:&secondary.wcid) ||
                   tx_calls!=1 || pm_unref!=1 || freed || traces!=1) {
            return fail("owner must resolve absent explicit link before shared drop");
        }
    } else if (!strcmp(argv[1], "eht_mt7922") || !strcmp(argv[1], "eht_mt7927")) {
        bool dut=!strcmp(argv[1], "eht_mt7927");
        dev.mt76.chip=dut?0x7927:0x7922;vif.mld=false;
        struct ieee80211_hw hw={.priv=&mphy,.dev=&dev};
        struct ieee80211_tx_control control={.sta=&sta};
        struct ieee80211_mgmt mgmt={.frame_control=0xd0,.u.action={.category=37}};
        skb.data=(void *)&mgmt;skb.len=sizeof(mgmt);skb.info.control.vif=&vif;
        mt792x_tx(&hw,&control,&skb);
        if (sent!=base || tx_calls!=1 || freed) return fail("non-MLD management TX changed");
        if (!!logs!=dut || traces!=(dut?1U:0U)) return fail("EHT diagnostics identity");
    } else if (!strcmp(argv[1], "sample_mt7922")) {
        dev.mt76.chip=0x7922; mt7925_mlo_sample_load(&dev);
        if (mt7925_mlo_load[2].last_ts) return fail("MT7922 sampled into global MLO state");
    } else if (!strcmp(argv[1], "sample_mt7927")) {
        mt7925_mlo_sample_load(&dev);
        if (mt7925_mlo_load[2].last_ts!=1000 || mt7925_mlo_load[2].last_tx_us!=1234) return fail("MT7927 sample lost");
    } else if (!strcmp(argv[1], "roc_mt7922") || !strcmp(argv[1], "roc_mt7927")) {
        bool dut=!strcmp(argv[1], "roc_mt7927");
        dev.mt76.chip=dut?0x7927:0x7922;
        mt792x_roc_timer(&phy.roc_timer);
        if (work_queued!=1 || !!logs!=dut) return fail("ROC diagnostic identity/ordinary work");
    } else if (!strcmp(argv[1], "worker_mt7922") || !strcmp(argv[1], "worker_mt7927")) {
        bool dut=!strcmp(argv[1], "worker_mt7927");
        dev.mt76.chip=dut?0x7927:0x7922;
        mphy.mac_work_count=1;
        mt792x_mac_work(&mphy.mac_work.work);
        if (survey_updates!=1 || mib_updates!=1 || status_checks!=1 || work_queued!=1 || lock_depth)
            return fail("ordinary shared watchdog behavior changed");
        if (mt7925_mlo_load[2].last_ts!=(dut?1000UL:0UL)) return fail("watchdog MLO sampling identity");
    } else if (!strcmp(argv[1], "header_null_vif")) {
        msta.vif=NULL;
        if (mt792x_sta_to_link(&msta,15)!=&msta.deflink) return fail("NULL vif default fallback");
        if (mt792x_sta_link_rcu(&msta,2)!=&msta.deflink) return fail("NULL vif RCU fallback");
    } else if (!strcmp(argv[1], "header_null_sta")) {
        if (mt792x_sta_to_link(NULL,15) || mt792x_sta_link_rcu(NULL,2)) return fail("NULL sta lookup");
    } else if (!strcmp(argv[1], "header_mt7922") || !strcmp(argv[1], "header_mt7927")) {
        bool dut=!strcmp(argv[1], "header_mt7927");
        dev.mt76.chip=dut?0x7927:0x7922;
        if (mt792x_sta_to_link(&msta,15)!=&msta.deflink || msta.mlo_tx_link_cursor)
            return fail("generic accessor must not own MLO round-robin");
        struct mt76_wcid *want=dut?&secondary.wcid:&msta.deflink.wcid;
        mt7925_mlo_force_tx_link=-1;skb.info.control.flags=2;
        if (mt7925_mlo_tx_select_link(&dev.mt76,&vif,&sta,&skb,base)!=want)
            return fail("explicit selection device isolation in actual owner");
        if (dut && mt7925_mlo_requested_link(&vif,&msta,15)!=&secondary)
            return fail("UNSPEC round-robin moved to actual owner");
        if (msta.mlo_tx_link_cursor!=(dut?2:0) || (!dut && logs)) return fail("UNSPEC state isolation");
        if (mt792x_sta_to_link(&msta,2)!=&secondary) return fail("explicit link lookup changed");
    } else return 2;
    printf("%s: PASS\n", argv[1]); return 0;
}
'''


class MloDeviceIsolation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "mt7925/mlo/tx.c").read_text()
        begin = source.index("int mt7925_mlo_force_tx_link =")
        end = source.index("static bool mt7925_mlo_preserve_data_da;")
        production = "\n".join([
            function("mt792x.h", "mt792x_sta_link_rcu"),
            function("mt792x.h", "mt792x_sta_to_link"),
            source[begin:end],
            function("mt792x_mac.c", "mt792x_mac_work"),
        ])
        selector = function("tx.c", "mt76_mlo_select_wcid")
        adapter = '\nstatic struct mt76_wcid *fixture_select(struct mt76_dev *dev, struct mt76_wcid *wcid, struct ieee80211_sta *sta, struct sk_buff *skb) { return mt76_mlo_select_wcid(dev, wcid, sta, skb);}\n'
        production += selector + adapter
        flag = source.index('static bool mt7925_mlo_preserve_data_da;')
        production += '\n' + source[flag:source.index(';', flag) + 1]
        production += '\n' + function('mt7925/mlo/tx.c', 'mt7925_mlo_tx_preserve_addr3')
        production += '\n' + function("mt792x_core.c", "mt792x_mlo_tx_link") + '\n' + function("mt792x_core.c", "mt792x_tx")
        production += '\n' + function("mt792x_core.c", "mt792x_roc_timer")
        print("Scope: verbatim MLO TX owner with shared per-device dispatch/accessors/watchdog; SHA256=" + hashlib.sha256(production.encode()).hexdigest(), flush=True)
        cls.tmp = tempfile.TemporaryDirectory(prefix="mt792x-isolation-")
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name)
        includes = '\n#include "mt792x_mlo_sched.h"\n#include "mt7925/mlo_tx_liveness.h"\n#include "mt7925/mlo_tx_primary.h"\n'
        (path / "fixture.c").write_text(PRELUDE + includes + production + MAIN)
        cls.executable = path / "fixture"
        build = subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", str(ROOT), str(path / "fixture.c"), "-o", str(cls.executable)], text=True, capture_output=True)
        if build.returncode:
            raise AssertionError(build.stderr)

    def check_case(self, case):
        result = subprocess.run([str(self.executable), case], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + f" exit={result.returncode}")


for _case in (
    "bulk_mt7922", "bulk_mt7922_mld", "bulk_non_mld", "bulk_null_vif", "bulk_null_sta", "bulk_mt7927",
    "bulk_invalid_forced", "bulk_invalid_tid", "bulk_invalid_control_port",
    "generic_mt7922_poisoned", "generic_other_poisoned", "generic_mt7927",
    "immediate_mt7922", "immediate_mt7927", "eht_mt7922", "eht_mt7927",
    "immediate_missing_sta", "immediate_missing_bmc", "immediate_missing_foreign",
    "immediate_missing_conf", "immediate_missing_peer",
    "immediate_writable_failure", "immediate_native_pm_queue",
    "immediate_inactive_sta", "immediate_invalid_sta", "immediate_no_live_sta",
    "immediate_inactive_bmc", "immediate_invalid_bmc", "immediate_no_live_bmc",
    "immediate_mt7925_sta_baseline", "immediate_mt7925_bmc_baseline", "immediate_mt7925_single",
    "immediate_mt7927_sta_cursor", "immediate_mt7927_bmc_cursor",
    "roc_mt7922", "roc_mt7927",
    "sample_mt7922", "sample_mt7927", "worker_mt7922", "worker_mt7927",
    "header_null_vif", "header_null_sta", "header_mt7922", "header_mt7927",
):
    setattr(MloDeviceIsolation, "test_" + _case, lambda self, case=_case: self.check_case(case))


MCU_PRELUDE = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define ETH_ALEN 6
#define EXPORT_SYMBOL_GPL(x)
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define unlikely(x) (x)
#define READ_ONCE(x) (x)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define MCU_UNI_CMD(x) (MCU_UNI_CMD_##x)
#define MCU_UNI_CMD_DEV_INFO_UPDATE 1
#define MCU_UNI_CMD_BSS_INFO_UPDATE 2
#define DEV_INFO_ACTIVE 0
#define UNI_BSS_INFO_BASIC 0
#define EXT_BSSID_START 16
#define HW_BSSID_0 0
#define NL80211_IFTYPE_STATION 2
#define NL80211_IFTYPE_AP 3
#define NL80211_IFTYPE_ADHOC 4
#define NL80211_IFTYPE_MESH_POINT 5
#define NL80211_IFTYPE_MONITOR 6
#define NL80211_IFTYPE_P2P_DEVICE 7
#define NL80211_IFTYPE_NAN 12
#define NL80211_IFTYPE_NAN_DATA 13
#define CONNECTION_NAN 0x60000
#define LINUX_VERSION_CODE 0x70300
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#define CONNECTION_INFRA_STA 0x10001
#define CONNECTION_INFRA_AP 0x10002
#define CONNECTION_P2P_GO 0x20002
#define CONNECTION_IBSS_ADHOC 0x40004
#define MT76_CONNAC_MCU_STATUS_WLAN_FAILURE 0xc0000001
#define KERN_INFO ""
#define DUMP_PREFIX_OFFSET 0
static unsigned mlo_logs, delay_calls, clock_reads, dumps;
static u32 delay_value;
static int mt76_mlo_diag_trace=1;
struct mt76_dev { unsigned chip; void *dev; };
struct mt76_phy { struct mt76_dev *dev; };
struct ieee80211_vif { int type; bool mld; };
struct ieee80211_bss_conf { struct ieee80211_vif *vif; u8 addr[6], link_id; };
struct mt76_vif_link { u8 omac_idx, band_idx, link_idx, idx, wmm_idx; };
struct mt76_wcid { unsigned idx; };
static bool is_mt7927(struct mt76_dev *dev) { return dev->chip==0x7927; }
static bool ieee80211_vif_is_mld(struct ieee80211_vif *vif) { return vif->mld; }
static void fixture_log(const char *format, ...) { (void)format; mlo_logs++; }
static void fixture_plain_log(void *dev, const char *format, ...) { (void)dev; (void)format; }
#define printk(...) fixture_log(__VA_ARGS__)
#define dev_dbg(...) fixture_plain_log(__VA_ARGS__)
#define dev_warn(...) fixture_plain_log(__VA_ARGS__)
#define WARN_ON(x) ((void)(x))
static u64 ktime_get_ns(void) { clock_reads++; return clock_reads*1000; }
static void mdelay(u32 delay) { delay_calls++; delay_value=delay; }
static void print_hex_dump(const char *level,const char *prefix,unsigned type,unsigned row,
                           unsigned group,const void *data,int len,bool ascii) {
    (void)level;(void)prefix;(void)type;(void)row;(void)group;(void)data;(void)len;(void)ascii;dumps++;
}
struct command { unsigned chip; int cmd,len; u8 data[64]; };
static struct command commands[4];
static unsigned sends;
static int fail_at, send_error=-EIO;
static int mt76_mcu_send_msg(struct mt76_dev *dev,int cmd,const void *data,int len,bool wait) {
    if (!dev || sends>=4 || len<0 || len>64 || !wait) return -EINVAL;
    commands[sends]=(struct command){.chip=dev->chip,.cmd=cmd,.len=len};
    memcpy(commands[sends].data,data,len); sends++;
    return fail_at==(int)sends?send_error:0;
}
'''


MCU_MAIN = r'''
static struct mt76_dev dev={.chip=0x7927};
static struct mt76_phy phy={.dev=&dev};
static struct ieee80211_vif vif={.type=NL80211_IFTYPE_STATION,.mld=true};
static struct ieee80211_bss_conf conf={.vif=&vif,.link_id=2,.addr={2,0x11,0x22,0x33,0x44,0x55}};
static struct mt76_vif_link link={.omac_idx=3,.band_idx=1,.link_idx=1,.idx=4,.wmm_idx=2};
static struct mt76_wcid wcid={.idx=21};
/* Hand-derived wire payloads: 4-byte header, 12-byte DEV / 32-byte BSS TLV. */
static const u8 expected_dev[]={3,1,0,0,0,0,12,0,1,1,2,0x11,0x22,0x33,0x44,0x55};
static const u8 expected_bss[]={4,0,0,0,0,0,32,0,1,3,3,1,1,0,1,0,1,2,0,0,0,0,0,0,21,0,0,0,0,0,21,0,0,0,0,1};
static int fail(const char *why) { fprintf(stderr,"FAIL: %s mode=%u sends=%u\n",why,mt76_mlo_bssadd_skip,sends); return 1; }
static bool payload(unsigned index, int cmd, bool enable, bool experimental) {
    u8 want[64]; unsigned len=cmd==1?16:36;
    memcpy(want,cmd==1?expected_dev:expected_bss,len);
    want[8]=enable;
    if (cmd==1) want[9]=link.link_idx; else want[35]=link.link_idx;
    if (experimental && link.link_idx) {
        switch(mt76_mlo_bssadd_skip) {
        case 3: if(cmd==2) want[10]=1;break;
        case 5: if(cmd==1) want[8]=0;break;
        case 6: if(cmd==2) want[16]=0;break;
        case 7: if(cmd==2) {want[30]=17;want[31]=0;}break;
        case 8: if(cmd==2) {want[30]=255;want[31]=255;}break;
        case 9: if(cmd==2) memset(want+12,0,4);break;
        }
    }
    return commands[index].chip==dev.chip && commands[index].cmd==cmd &&
           commands[index].len==(int)len && !memcmp(commands[index].data,want,len);
}
static void reset(unsigned chip,unsigned mode,bool secondary) {
    dev.chip=chip;link.link_idx=secondary;mt76_mlo_bssadd_skip=mode;mt76_mlo_bssadd_delay_ms=17;
    sends=mlo_logs=delay_calls=clock_reads=dumps=0;delay_value=0;fail_at=0;send_error=-EIO;
    memset(commands,0,sizeof(commands));mt76_mlo_deferred_pending=false;
}
static int ordinary(bool errors) {
    for(unsigned mode=0;mode<=10;mode++) for(unsigned secondary=0;secondary<2;secondary++)
    for(unsigned enable=0;enable<2;enable++) {
        reset(0x7922,mode,secondary);
        int expected=0;unsigned count=2;
        if(errors) {fail_at=enable?2:1;expected=-EIO;count=enable?2:1;}
        int result=mt76_connac_mcu_uni_add_dev(&phy,&conf,&link,&wcid,enable);
        if(result!=expected || sends!=count) return fail("MT7922 command count/error");
        if(!payload(0,enable?1:2,enable,false) || (count==2 && !payload(1,enable?2:1,enable,false)))
            return fail("MT7922 command order/payload");
        if(mlo_logs || delay_calls || clock_reads || dumps || mt76_mlo_deferred_pending)
            return fail("MT7922 experiment side effect");
    }
    return 0;
}
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    (void)is_mt7927; /* Pre-guard source has no device-identity consumer. */
    unsigned specific_mode;
    if(!strcmp(argv[1],"mcu_mt7922")) {if(ordinary(false)) return 1;}
    else if(!strcmp(argv[1],"mcu_mt7922_errors")) {if(ordinary(true)) return 1;}
    else if(sscanf(argv[1],"mcu_mt7922_mode_%u",&specific_mode)==1) {
        if(specific_mode<1 || specific_mode>10) return 2;
        for(unsigned enable=0;enable<2;enable++) {
            reset(0x7922,specific_mode,true);mt76_mlo_diag_trace=0;
            mt76_mlo_bssadd_delay_ms=specific_mode==7?17:0;
            if(mt76_connac_mcu_uni_add_dev(&phy,&conf,&link,&wcid,enable) || sends!=2 ||
               !payload(0,enable?1:2,enable,false) || !payload(1,enable?2:1,enable,false) ||
               mt76_mlo_deferred_pending || delay_calls) return fail("MT7922 poisoned knob changes command path");
        }
    }
    else if(!strcmp(argv[1],"mcu_mt7922_deactivate")) {
        for(unsigned mode=0;mode<=10;mode++) {
            reset(0x7922,mode,true);fail_at=1;send_error=(int)0xc0000001;
            if(mt76_connac_mcu_uni_add_dev(&phy,&conf,&link,&wcid,false) || sends!=2 ||
               !payload(0,2,false,false) || !payload(1,1,false,false) || mlo_logs || delay_calls)
                return fail("MT7922 inactive-BSS error normalization");
        }
    } else if(!strcmp(argv[1],"mcu_mt7927")) {
        for(unsigned mode=0;mode<=10;mode++) {
            reset(0x7927,mode,true);
            if(mt76_connac_mcu_uni_add_dev(&phy,&conf,&link,&wcid,true)) return fail("MT7927 add");
            unsigned count=(mode==1 || mode==2 || mode==10)?1:2;
            int first=(mode==1 || mode==4)?2:1;
            if(sends!=count || !payload(0,first,true,true) ||
               (count==2 && !payload(1,mode==4?1:2,true,true))) return fail("MT7927 experiment lost");
            if(delay_calls!=1 || delay_value!=17 || !mlo_logs) return fail("MT7927 delay/trace lost");
            if(mode==10) {
                if(!mt76_mlo_deferred_pending) return fail("MT7927 defer lost");
                mt76_mlo_bssinfo_deferred_send();
                if(mt76_mlo_deferred_pending || sends!=2 || !payload(1,2,true,true)) return fail("MT7927 deferred payload/send");
            } else if(mt76_mlo_deferred_pending) return fail("unexpected deferred state");
        }
    } else if(!strcmp(argv[1],"mcu_mt7927_primary")) {
        for(unsigned mode=0;mode<=10;mode++) {
            reset(0x7927,mode,false);
            if(mt76_connac_mcu_uni_add_dev(&phy,&conf,&link,&wcid,true) || sends!=2 ||
               !payload(0,1,true,false) || !payload(1,2,true,false) || delay_calls || mt76_mlo_deferred_pending)
                return fail("MT7927 primary behavior changed");
        }
    } else if(!strcmp(argv[1],"deferred_mt7922") || !strcmp(argv[1],"deferred_null")) {
        reset(0x7922,10,true);mt76_mlo_deferred_pending=true;
        mt76_mlo_deferred_dev=!strcmp(argv[1],"deferred_null")?NULL:&dev;
        mt76_mlo_deferred_cmd=2;mt76_mlo_deferred_len=36;
        memcpy(mt76_mlo_deferred_payload,expected_bss,36);
        mt76_mlo_bssinfo_deferred_send();
        if(sends || mlo_logs || mt76_mlo_deferred_pending) return fail("foreign/NULL deferred device sent/logged");
    } else return 2;
    printf("%s: PASS\n",argv[1]);return 0;
}
'''


class SharedMcuDeviceIsolation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        header = (ROOT / "mt76_connac_mcu.h").read_text()
        start = header.index("struct mt76_connac_bss_basic_tlv {")
        basic = header[start:header.index("} __packed;", start) + len("} __packed;")]
        source = (ROOT / "mt76_connac_mcu.c").read_text()
        start = source.index("u8 mt76_mlo_bssadd_skip;")
        production = basic + '\n' + function("mt76_connac_mcu.h", "mt76_connac_mcu_bss_deact_err") + '\n' + source[start:source.index("EXPORT_SYMBOL_GPL(mt76_connac_mcu_uni_add_dev);", start)]
        print("Scope: verbatim shared MCU add-device/deferred/error helper; SHA256=" + hashlib.sha256(production.encode()).hexdigest(), flush=True)
        cls.tmp = tempfile.TemporaryDirectory(prefix="mt76-mcu-isolation-")
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name)
        (path / "fixture.c").write_text(MCU_PRELUDE + production + MCU_MAIN)
        cls.executable = path / "fixture"
        build = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(path / "fixture.c"), "-o", str(cls.executable)], text=True, capture_output=True)
        if build.returncode:
            raise AssertionError(build.stderr)

    def check_case(self, case):
        result = subprocess.run([str(self.executable), case], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + f" exit={result.returncode}")


for _case in (
    "mcu_mt7922", "mcu_mt7922_errors", "mcu_mt7922_deactivate", "mcu_mt7927", "mcu_mt7927_primary",
    "deferred_mt7922", "deferred_null",
):
    setattr(SharedMcuDeviceIsolation, "test_" + _case, lambda self, case=_case: self.check_case(case))

for _mode in range(1, 11):
    _case = f"mcu_mt7922_mode_{_mode}"
    setattr(SharedMcuDeviceIsolation, "test_" + _case, lambda self, case=_case: self.check_case(case))


class ActiveLinkRequestErrors(unittest.TestCase):
    """Catch the debugfs boundary swallowing real controller refusals.

    The production iterator and setter execute unchanged; only interface
    enumeration and the asynchronous controller boundary are doubled.
    """

    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'mt7925/debugfs.c').read_text()
        start = source.index('struct mt7925_set_links_ctx {')
        context = source[start:source.index('\n};', start) + 3]
        prelude = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint64_t u64;
#define NL80211_IFTYPE_STATION 2
#define IEEE80211_IFACE_ITER_RESUME_ALL 1
#define pr_info(...) fprintf(stderr, __VA_ARGS__)
struct ieee80211_vif { int type; bool mld; u16 active_links, valid_links, dormant_links; };
struct mt792x_dev { int identity; };
static struct ieee80211_vif vif;
static bool present;
static int controller_result, controller_calls;
static struct mt792x_dev *expected_dev;
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v) { return v->mld; }
static void *mt76_hw(struct mt792x_dev *d) { return d; }
static void ieee80211_iterate_active_interfaces(void *hw, int flags,
        void (*callback)(void *, u8 *, struct ieee80211_vif *), void *ctx) {
    (void)hw; (void)flags;
    if (present) callback(ctx, NULL, &vif);
}
static int mt7925_mlo_request_active_links_negotiated(struct mt792x_dev *d,
        struct ieee80211_vif *v, u16 links) {
    controller_calls++;
    if (d != expected_dev || v != &vif || links != 1) return -EINVAL;
    return controller_result;
}
'''
        cases = r'''
int main(int argc, char **argv) {
    if (argc != 2) return 99;
    int mode = atoi(argv[1]);
    struct mt792x_dev dev = {7}; expected_dev = &dev;
    vif = (struct ieee80211_vif){.type=NL80211_IFTYPE_STATION, .mld=true};
    present = mode != 4;
    if (mode == 5) vif.type = 3;
    if (mode == 6) vif.mld = false;
    const int results[] = {0, -EACCES, -ENODATA, -ETIMEDOUT, 0, 0, 0};
    controller_result = results[mode];
    int ret = mt7925_mlo_active_links_set(&dev, 0x10001);
    printf("%d %d\n", ret, controller_calls);
    return 0;
}
'''
        cls.tmp = tempfile.TemporaryDirectory(prefix='mt7927-request-errors-')
        cls.exe = Path(cls.tmp.name) / 'fixture'
        c = Path(cls.tmp.name) / 'fixture.c'
        c.write_text(prelude + context + '\n' +
                     function('mt7925/debugfs.c', 'mt7925_set_links_iter') + '\n' +
                     function('mt7925/debugfs.c', 'mt7925_mlo_active_links_set') + cases)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', str(c), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_request_results_and_absent_eligible_interface(self):
        for mode, expected in ((0, (0, 1)), (1, (-13, 1)),
                               (2, (-61, 1)), (3, (-110, 1)),
                               (4, (-19, 0)), (5, (-19, 0)), (6, (-19, 0))):
            with self.subTest(mode=mode):
                result = subprocess.run([str(self.exe), str(mode)],
                                        check=True, capture_output=True, text=True)
                self.assertEqual(tuple(map(int, result.stdout.split())), expected)


class CaptureContextIsolation(unittest.TestCase):
    def test_actual_capture_context_is_privileged_and_only_for_exact_dut(self):
        source = (ROOT / 'mt7925/debugfs.c').read_text()
        self.assertTrue('mt7925_mlo_capture_context_read(' in source,
                      'DUT-only RX probes need a read-only exact-context source')
        body = function('mt7925/debugfs.c', 'mt7925_mlo_capture_context_read')
        prelude = r'''
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#define CAP_SYS_ADMIN 21
struct device { const char *name; void *driver_data; };
struct wiphy { const char *name; };
struct ieee80211_hw { struct wiphy *wiphy; };
struct mt76_dev { struct device *dev; struct ieee80211_hw *hw; bool chip; };
struct mt792x_dev { struct mt76_dev mt76; };
struct seq_file { void *private; };
static bool privileged;
static void *dev_get_drvdata(void *d) { return ((struct device *)d)->driver_data; }
static bool capable(int cap) { return privileged && cap == CAP_SYS_ADMIN; }
static bool is_mt7927(struct mt76_dev *d) { return d->chip; }
static const char *dev_name(struct device *d) { return d->name; }
static const char *wiphy_name(struct wiphy *w) { return w->name; }
static void seq_printf(struct seq_file *s, const char *fmt, ...) {
 char portable[1024]; size_t n=0;
 (void)s;
 /* Linux %px emits raw pointers; portable %p is enough to test identity. */
 for(size_t i=0;fmt[i];i++) {
  if(fmt[i]=='%' && fmt[i+1]=='p' && fmt[i+2]=='x') {
   portable[n++]='%';portable[n++]='p';i+=2;
  } else portable[n++]=fmt[i];
 }
 portable[n]=0;va_list ap;va_start(ap,fmt);vprintf(portable,ap);va_end(ap);
}
'''
        cases = r'''
int main(int argc, char **argv) {
 if(argc!=2)return 99;
 int mode=atoi(argv[1]);
 struct device pci={mode==2 ? "0000:08:00.0" : "0000:07:00.0", NULL};
 struct wiphy wiphy={"phy8"};struct ieee80211_hw hw={&wiphy};
 struct mt792x_dev dev={.mt76={&pci,&hw,mode!=3}};
 pci.driver_data=mode==4 ? NULL : &dev;
 struct seq_file s={&pci};privileged=mode!=1;
 if(mode==5)dev.mt76.hw=NULL;
 int ret=mt7925_mlo_capture_context_read(&s,NULL);
 fprintf(stderr,"result=%d dev=%p mt76=%p hw=%p\n",ret,(void *)&dev,(void *)&dev.mt76,(void *)&hw);
 return ret==0 ? 0 : (ret==-EPERM ? 10 : (ret==-ENODEV ? 11 : 99));
}
'''
        with tempfile.TemporaryDirectory(prefix='mt7927-capture-context-') as tmp:
            c, exe = Path(tmp) / 'fixture.c', Path(tmp) / 'fixture'
            c.write_text(prelude + body + cases)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', str(c), '-o', str(exe)], check=True)
            for mode, code in ((0, 0), (1, 10), (2, 11), (3, 11), (4, 11), (5, 11)):
                result = subprocess.run([str(exe), str(mode)], capture_output=True, text=True)
                self.assertEqual(result.returncode, code, result.stderr)
                if mode:
                    self.assertEqual(result.stdout, '', 'refusal must not disclose pointers')
                else:
                    got = json.loads(result.stdout)
                    self.assertEqual((got['version'], got['bdf'], got['phy']),
                                     (1, '0000:07:00.0', 'phy8'))
                    for name in ('mt792x_dev', 'mt76_dev', 'hw'):
                        expected = re.search(name.replace('mt792x_dev', 'dev').replace('mt76_dev', 'mt76')
                                             + r'=(0x[0-9a-f]+)', result.stderr)[1]
                        self.assertEqual(int(got[name], 16), int(expected, 16))
                    self.assertNotIn('key', got)
                    self.assertNotIn('association_epoch', got)


class PairProfilePolicy(unittest.TestCase):
    """Execute real profile config/gates; no simulated firmware admission.

    Catches enabled-as-supported, bypass through the legacy enable control,
    validation surviving an invalidation, and cross-device config writes.
    """

    def test_rssi_event_updates_only_the_armed_link(self):
        body = function('mt7925/mlo/steering.c', 'mt7927_mlo_steering_rssi_event')
        body = body.replace('(void *)vif->drv_priv', 'vif->private')
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8; typedef int32_t s32;
#define IEEE80211_MLD_MAX_NUM_LINKS 3
#define MT792X_MLO_RSSI_UNKNOWN (-128)
#define S8_MIN (-128)
#define S8_MAX 127
#define BIT(n) (1U << (n))
#define clamp_t(t,v,lo,hi) ((v)<(lo)?(lo):((v)>(hi)?(hi):(v)))
#define dev_info(...) ((void)0)
static unsigned long jiffies=123;
struct mt792x_dev { struct { void *dev; } mt76; };
struct phy { struct mt792x_dev *dev; };
struct mt792x_vif { struct phy *phy; bool mlo_rssi_monitor_enabled;
 u8 mlo_rssi_monitor_link_id; int mlo_link_rssi[3];
 unsigned long mlo_link_rssi_jiffies[3]; unsigned valid_links; };
struct ieee80211_vif { struct mt792x_vif *private; bool mld; };
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v) { return v->mld; }
'''
        cases = r'''
int main(void) {
 struct mt792x_dev d={0}; struct phy phy={&d};
 struct mt792x_vif m={.phy=&phy,.valid_links=7};
 struct ieee80211_vif v={&m,true};
 mt7927_mlo_steering_rssi_event(&v,-45);
 if(m.mlo_link_rssi_jiffies[0]) return 1;
 m.mlo_rssi_monitor_enabled=true;m.mlo_rssi_monitor_link_id=1;
 mt7927_mlo_steering_rssi_event(&v,-42);
 if(m.mlo_link_rssi[1]!=-42 || m.mlo_link_rssi_jiffies[1]!=123 || m.mlo_link_rssi_jiffies[0]) return 2;
 mt7927_mlo_steering_rssi_event(&v,-999);
 if(m.mlo_link_rssi[1]!=-128) return 3;
 mt7927_mlo_steering_rssi_event(&v,999);
 if(m.mlo_link_rssi[1]!=127) return 4;
 m.mlo_rssi_monitor_link_id=255;
 mt7927_mlo_steering_rssi_event(&v,-99);
 v.mld=false;m.mlo_rssi_monitor_link_id=2;
 mt7927_mlo_steering_rssi_event(&v,-99);
 if(m.mlo_link_rssi_jiffies[2]) return 5;
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='mt7927-rssi-owner-') as tmp:
            c, exe = Path(tmp)/'rssi.c', Path(tmp)/'rssi'
            c.write_text(prefix + body + cases)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra',
                            '-Wno-unused-variable', '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_association_prepare_does_not_weaken_external_admission(self):
        h = (ROOT/'mt7925/mlo/pairs.h').read_text()
        declarations = h[h.index('enum mt7927_mlo_support'):h.index('/* Kernel integration')]
        bodies = '\n'.join(function('mt7925/mlo/pairs.c', name) for name in (
            'mt7927_mlo_pair_index', 'mt7927_mlo_profiles_init',
            'mt7927_mlo_profiles_admit', 'mt7927_mlo_pairs_admit_phase',
            'mt7927_mlo_pairs_admit', 'mt7927_mlo_pairs_prepare_association'))
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define hweight16(x) __builtin_popcount((unsigned)(x))
#define spin_lock_irqsave(lock,flags) ((void)(lock),(flags)=0)
#define spin_unlock_irqrestore(lock,flags) ((void)(lock),(void)(flags))
'''
        seams = r'''
struct mt7925_mlo_vif { int pair_lock; struct mt7927_mlo_profiles profiles; };
struct ieee80211_vif { struct { bool assoc; } cfg; u16 valid_links,usable;
 bool supported,mld; struct mt7925_mlo_vif private; };
static bool mt7927_mlo_vif_supported(struct ieee80211_vif *v) { return v->supported; }
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v) { return v->mld; }
static struct mt7925_mlo_vif *mt7927_mlo_private(struct ieee80211_vif *v) { return &v->private; }
static u16 ieee80211_vif_usable_links(struct ieee80211_vif *v) { return v->usable; }
'''
        cases = r'''
int main(void) {
 struct ieee80211_vif v={.valid_links=7,.usable=7,.supported=true,.mld=true};
 mt7927_mlo_profiles_init(&v.private.profiles,true);
 if(mt7927_mlo_pairs_admit(&v,3,false)!=-ENOTCONN) return 1;
 if(mt7927_mlo_pairs_prepare_association(&v,3)) return 2;
 if(v.cfg.assoc || v.private.profiles.authorized || v.private.profiles.pair[0].current_validated) return 3;
 v.private.profiles.pair[0].enabled=false;
 if(mt7927_mlo_pairs_prepare_association(&v,3)!=-EACCES) return 4;
 if(mt7927_mlo_pairs_admit(&v,3,false)!=-ENOTCONN) return 5;
 v.cfg.assoc=true;
 if(mt7927_mlo_pairs_admit(&v,3,false)!=-EACCES) return 6;
 v.private.profiles.pair[0].enabled=true;v.usable=5;
 if(mt7927_mlo_pairs_prepare_association(&v,3)!=-EAGAIN) return 7;
 v.usable=7;v.valid_links=5;
 if(mt7927_mlo_pairs_prepare_association(&v,3)!=-EINVAL) return 8;
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='mt7927-admission-phase-') as tmp:
            c, exe = Path(tmp)/'phase.c', Path(tmp)/'phase'
            c.write_text(prefix + declarations + seams + bodies + cases)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_pair_configuration_and_validation_boundary(self):
        header = ROOT / 'mt7925/mlo/pairs.h'
        source = ROOT / 'mt7925/mlo/pairs.c'
        self.assertTrue(header.exists() and source.exists(),
                        'dedicated pair profile implementation is missing')
        self.assertTrue('void mt7927_mlo_profile_note(' in source.read_text(),
                      'bounded per-pair request/failure history is missing')
        self.assertTrue('bool mt7927_mlo_context_link_matches(' in source.read_text(),
                        'diagnostics must reject another link context')
        h = header.read_text()
        self.assertIn('u32 prior_implementation_memory;', h,
                      'pair history must distinguish prior implementation from current acceptance')
        declarations = h[h.index('enum mt7927_mlo_support'):h.index('/* Kernel integration')]
        bodies = '\n'.join(function('mt7925/mlo/pairs.c', name) for name in (
            'mt7927_mlo_pair_index', 'mt7927_mlo_profiles_init',
            'mt7927_mlo_profiles_invalidate', 'mt7927_mlo_profiles_can_steer',
            'mt7927_mlo_profiles_configure', 'mt7927_mlo_profile_note',
            'mt7927_mlo_context_link_matches', 'mt7927_mlo_profiles_admit'))
        prelude = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16;
typedef uint32_t u32; typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define strscpy(a,b,n) snprintf(a,n,"%s",b)
#define hweight16(x) __builtin_popcount((unsigned)(x))
'''
        cases = r'''
int main(void) {
 struct mt7927_mlo_profiles p, other;
 unsigned checks=0, failures=0;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line%d: %s\n",__LINE__,#x); } } while(0)
 mt7927_mlo_profiles_init(&p, true);
 CHECK(mt7927_mlo_pair_index(3)==0 && mt7927_mlo_pair_index(5)==1 && mt7927_mlo_pair_index(6)==2);
 CHECK(mt7927_mlo_pair_index(7)==-EINVAL && mt7927_mlo_pair_index(0)==-EINVAL);
 CHECK(mt7927_mlo_context_link_matches(1,1));
 CHECK(!mt7927_mlo_context_link_matches(1,2) && !mt7927_mlo_context_link_matches(1,-1));
 CHECK(p.pair[0].enabled && p.pair[1].enabled);
 /* the single-radio 5+6 GHz profile is not a steering mode: off by default, never entered
  * by association or steering, still reachable through the explicit control for diagnostics */
 CHECK(!p.pair[2].enabled);
 CHECK(mt7927_mlo_profiles_admit(&p,6,7,7,true,false)==-EACCES);
 CHECK(p.pair[0].support==MT7927_MLO_SUPPORT_UNKNOWN && !p.pair[0].current_validated);
 CHECK(p.pair[1].historical_memory==927 && p.pair[0].historical_memory==0 && p.pair[2].historical_memory==0);
 CHECK(p.pair[0].prior_implementation_memory==933 && p.pair[1].prior_implementation_memory==932 && p.pair[2].prior_implementation_memory==931);
 CHECK(mt7927_mlo_profiles_admit(&p,3,7,7,true,false)==0);
 CHECK(mt7927_mlo_profiles_admit(&p,3,7,7,false,false)==-ENOTCONN);
 CHECK(mt7927_mlo_profiles_admit(&p,7,7,7,true,false)==-EINVAL);
 CHECK(mt7927_mlo_profiles_admit(&p,5,3,7,true,false)==-EINVAL);
 CHECK(mt7927_mlo_profiles_admit(&p,0,7,7,true,false)==-EINVAL);
 CHECK(mt7927_mlo_profiles_admit(&p,3,7,5,true,false)==-EAGAIN);
 CHECK(mt7927_mlo_profiles_admit(&p,3,7,5,true,true)==0);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_ENABLE,1)==-EAGAIN);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_PAIR03,0)==0 && !p.pair[0].enabled && p.pair[1].enabled);
 CHECK(mt7927_mlo_profiles_admit(&p,3,7,7,true,false)==-EACCES);
 CHECK(mt7927_mlo_profiles_admit(&p,4,7,7,true,false)==0); /* recovery is not a disabled pair */
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_PAIR03,2)==-EINVAL);
 CHECK(mt7927_mlo_profiles_configure(&p,(enum mt7927_mlo_control)-1,0)==-EINVAL);
 CHECK(mt7927_mlo_profiles_configure(&p,(enum mt7927_mlo_control)99,0)==-EINVAL);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_INTERVAL,999)==-ERANGE);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_INTERVAL,600001)==-ERANGE);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_INTERVAL,1000)==0 && p.interval_ms==1000);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_POLICY,1)==-EOPNOTSUPP);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_FALLBACK,7)==-EINVAL);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_FALLBACK,3)==-EOPNOTSUPP);
 /* Steering gate v2: authorization plus two usable SIMULTANEOUS-mode profiles
  * (link bitmaps 0x3 and 0x5). The single-radio 5+6 GHz profile (0x6) is not a
  * steering mode and gates nothing; a mode is proven by its own authenticated
  * commit and guarded by validation, rollback and backoff. These inputs
  * exercise the gate, NOT hardware certification. */
 CHECK(!mt7927_mlo_profiles_can_steer(&p)); /* no authorization, no execution */
 p.authorized=true;
 CHECK(!mt7927_mlo_profiles_can_steer(&p)); /* 0x3 is still disabled here: one candidate */
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_PAIR03,1)==0);
 CHECK(mt7927_mlo_profiles_can_steer(&p)); /* two simultaneous modes, no stationary pre-gate */
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_PAIR06,0)==0);
 CHECK(mt7927_mlo_profiles_can_steer(&p)); /* the 5+6 profile does not gate steering */
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_PAIR06,1)==0);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_PAIR05,0)==0);
 CHECK(!mt7927_mlo_profiles_can_steer(&p)); /* one candidate cannot steer */
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_PAIR05,1)==0);
 p.pair[1].support=MT7927_MLO_SUPPORT_UNSUPPORTED;
 CHECK(!mt7927_mlo_profiles_can_steer(&p)); /* a mode proven unsupported is out */
 p.pair[1].support=MT7927_MLO_SUPPORT_UNKNOWN;
 CHECK(mt7927_mlo_profiles_can_steer(&p));
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_ENABLE,1)==0);
 p.pair[0].current_validated=true; p.pair[1].current_validated=true;
 p.pair[0].last_error=-110; p.pair[0].failures=1;
 mt7927_mlo_profiles_invalidate(&p);
 CHECK(!p.pair[0].current_validated && !p.pair[1].current_validated); /* validation is per epoch */
 p.authorized=false;
 CHECK(!mt7927_mlo_profiles_can_steer(&p)); /* security loss revokes steering */
 CHECK(p.pair[1].historical_memory==927 && p.pair[0].last_error==-110 && p.pair[0].failures==1);
 CHECK(p.pair[0].prior_implementation_memory==933 && p.pair[1].prior_implementation_memory==932 && p.pair[2].prior_implementation_memory==931);
 CHECK(mt7927_mlo_profiles_configure(&p,MT7927_MLO_CTL_ENABLE,0)==0);
 mt7927_mlo_profiles_init(&other, false);
 CHECK(mt7927_mlo_profiles_admit(&other,3,7,7,true,false)==-ENODEV);
 CHECK(mt7927_mlo_profiles_configure(&other,MT7927_MLO_CTL_INTERVAL,5000)==-ENODEV);
 CHECK(!mt7927_mlo_profiles_can_steer(&other) && !other.pair[1].historical_memory);
 CHECK(p.interval_ms==1000); /* other device cannot mutate this config */
 p.association_epoch=88;
 mt7927_mlo_profile_note(&p,5,0,"request",123,false);
 CHECK(p.pair[1].last_attempt_ns==123 && p.pair[0].last_attempt_ns==0);
 p.pair[0].current_validated=true;p.pair[2].current_validated=true;
 mt7927_mlo_profile_note(&p,5,-110,"roc_timeout",124,false);
 CHECK(p.pair[0].current_validated && p.pair[2].current_validated); /* failure belongs to05 */
 CHECK(p.pair[1].last_error==-110 && !strcmp(p.pair[1].last_reason,"roc_timeout"));
 CHECK(p.pair[1].failure[0].error==-110 && p.pair[1].failure[0].ns==124);
 CHECK(p.pair[1].failure[0].epoch==88 && p.pair[1].last_attempt_epoch==88);
 mt7927_mlo_profile_note(&p,5,0,"outer_activation_return",125,true);
 CHECK(p.pair[1].last_core_activation_ns==125 && p.pair[1].last_error==-110);
 CHECK(p.pair[1].last_core_activation_epoch==88);
 CHECK(!p.pair[1].current_validated && p.pair[1].historical_memory==927);
 for(unsigned i=0;i<5;i++) mt7927_mlo_profile_note(&p,3,-95,"MLSR_HIGH_BAND_SWITCH_REQUIRED",200+i,false);
 CHECK(p.pair[0].failures==6 && p.pair[0].failure[1].ns==204);
 mt7927_mlo_profile_note(&p,4,-110,"partial_mask_not_pair",300,false);
 CHECK(p.pair[1].last_error==-110 && p.pair[2].failures==0);
 /* failure backoff: base = steering cooldown, doubles per consecutive failure, capped */
 CHECK(p.pair[0].consecutive_failures==5);
 CHECK(p.pair[0].backoff_until_ns==204+(MT7927_MLO_BACKOFF_BASE_MS*1000000ULL<<MT7927_MLO_BACKOFF_MAX_SHIFT));
 mt7927_mlo_profile_note(&p,3,-95,"MLSR_HIGH_BAND_SWITCH_REQUIRED",900,false);
 CHECK(p.pair[0].consecutive_failures==6);
 CHECK(p.pair[0].backoff_until_ns==900+(MT7927_MLO_BACKOFF_BASE_MS*1000000ULL<<MT7927_MLO_BACKOFF_MAX_SHIFT)); /* capped */
 CHECK(p.pair[1].consecutive_failures==1 && p.pair[1].backoff_until_ns==124+MT7927_MLO_BACKOFF_BASE_MS*1000000ULL);
 /* a request that never started, and a started one that is not a failure, never back off */
 mt7927_mlo_profile_note(&p,5,0,"request",950,false);
 CHECK(p.pair[1].consecutive_failures==1);
 /* an authenticated commit proves the mode and clears its backoff */
 mt7927_mlo_profile_note(&p,3,0,"commit",1000,true);
 CHECK(p.pair[0].current_validated && p.pair[0].support==MT7927_MLO_SUPPORT_SUPPORTED);
 CHECK(p.pair[0].consecutive_failures==0 && p.pair[0].backoff_until_ns==0);
 CHECK(p.pair[1].consecutive_failures==1 && !p.pair[1].current_validated); /* other mode untouched */
 /* a later failure revokes that validation */
 mt7927_mlo_profile_note(&p,3,-5,"validation_failed",1100,false);
 CHECK(!p.pair[0].current_validated && p.pair[0].consecutive_failures==1);
 printf("pair-profile checks=%u failures=%u\n",checks,failures);
 return failures!=0;
}
'''
        with tempfile.TemporaryDirectory(prefix='mt7927-pair-policy-') as tmp:
            c, exe = Path(tmp)/'pair.c', Path(tmp)/'pair'
            c.write_text(prelude + declarations + bodies + cases)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_controller_does_not_poll_while_disabled_or_cross_devices(self):
        path = ROOT / 'mt7925/mlo/steering.c'
        self.assertTrue(path.exists(), 'single controller has not been extracted')
        body = function('mt7925/mlo/steering.c', 'mt7927_mlo_steering_kick')
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
struct ieee80211_vif { struct { bool assoc; } cfg; bool mld;
 struct { struct { struct { unsigned chip; } mt76; } *dev; } *phy;
 bool mlo_auto_steering, mlo_steer_validating;
 struct { bool enabled; } mlo_completion; int mlo_steer_work; };
#define mt792x_vif ieee80211_vif
#define drv_priv cfg_unused
/* Prefix-only seam maps the actual VIF private lookup, not scheduling. */
#define READ_ONCE(x) (x)
static unsigned queues, last_delay;
static bool is_mt7927(void *d) { return *(unsigned *)d==0x7927; }
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v) { return v->mld; }
static void *mt76_hw(void *d) { return d; }
static unsigned mt7927_mlo_steering_interval(struct ieee80211_vif *v) { (void)v; return 5000; }
static bool mt7927_mlo_pairs_can_steer(struct ieee80211_vif *v) { (void)v; return true; }
static void ieee80211_queue_delayed_work(void *hw, void *work, unsigned delay) { (void)hw;(void)work;queues++;last_delay=delay; }
static bool autostart_arm;
static void mt7927_mlo_pairs_autostart(struct ieee80211_vif *v) { if (autostart_arm) v->mlo_auto_steering = true; }
'''
        # Replace only the private-storage address seam; production branch
        # conditions and scheduling arguments execute verbatim.
        body = body.replace('(void *)vif->drv_priv', 'vif')
        suffix = r'''
int main(void) {
 struct ieee80211_vif v={.cfg={true},.mld=true,.mlo_completion={true}};
 __typeof__(*v.phy) phy={0}; __typeof__(*phy.dev) dev={0};
 phy.dev=&dev;v.phy=&phy;dev.mt76.chip=0x7927;
 mt7927_mlo_steering_kick(&v);if(queues)return 1;
 v.mlo_auto_steering=true;mt7927_mlo_steering_kick(&v);
 if(queues!=1 || last_delay!=5000)return 2;
 v.mlo_auto_steering=false;v.mlo_steer_validating=true;
 mt7927_mlo_steering_kick(&v);if(queues!=2)return 3;
 dev.mt76.chip=0x7922;mt7927_mlo_steering_kick(&v);if(queues!=2)return 4;
 dev.mt76.chip=0x7927;v.mlo_completion.enabled=false;
 mt7927_mlo_steering_kick(&v);if(queues!=2)return 5;
 /* wanted-but-stopped steering resumes through the single scheduler, once the
  * pairs layer re-arms it; without that re-arm nothing is queued */
 v.mlo_completion.enabled=true;v.mlo_auto_steering=false;v.mlo_steer_validating=false;
 mt7927_mlo_steering_kick(&v);if(queues!=2)return 6;
 autostart_arm=true;mt7927_mlo_steering_kick(&v);if(queues!=3)return 7;
 puts("controller kick: 7 cases PASS");return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='mt7927-controller-') as tmp:
            c, exe = Path(tmp)/'kick.c', Path(tmp)/'kick'
            c.write_text(prefix + body + suffix)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_request_history_includes_outer_refusal_and_ttlm_failure(self):
        h = (ROOT/'mt7925/mlo/pairs.h').read_text()
        declarations = h[h.index('enum mt7927_mlo_support'):h.index('/* Kernel integration')]
        policy = '\n'.join(function('mt7925/mlo/pairs.c', name) for name in (
            'mt7927_mlo_pair_index', 'mt7927_mlo_profiles_init',
            'mt7927_mlo_profiles_invalidate', 'mt7927_mlo_profile_note'))
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define strscpy(a,b,n) snprintf(a,n,"%s",b)
#define READ_ONCE(x) (x)
#define NL80211_IFTYPE_STATION 2
#define dev_info(...) ((void)0)
#define IEEE80211_TTLM_NUM_TIDS 8
#define BIT(n) (1U<<(n))
'''
        seams = r'''
struct mt792x_dev { int mt76; };
struct ieee80211_vif { u16 valid_links,active_links,dormant_links;unsigned type;struct mt7927_mlo_profiles p; };
struct mt792x_mlo_ticket { int unused; };
struct cfg80211_ttlm_params { u16 dlink[8],ulink[8]; };
static bool mt7925_test_mt7927_mlo_ttlm_removal;
static int transport_error;
static int admission_error;
static int __attribute__((unused)) mt7927_mlo_pairs_admit(struct ieee80211_vif *v,u16 p,bool negotiate)
{(void)v;(void)p;(void)negotiate;return admission_error;}
static unsigned now;
static bool is_mt7927(void *d) {(void)d;return true;}
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v){(void)v;return true;}
static void mt7927_mlo_pairs_note(struct ieee80211_vif *v,u16 pair,int e,const char *r,bool done)
{mt7927_mlo_profile_note(&v->p,pair,e,r,++now,done);}
static int mt7925_mlo_secure_gate(struct mt792x_dev *d,struct ieee80211_vif *v,u16 p,const char *r)
{(void)d;(void)v;(void)p;(void)r;return 0;}
static int mt7925_mlo_completion_request_after(struct ieee80211_vif *v,u16 p,const struct mt792x_mlo_ticket *a,struct mt792x_mlo_ticket *b)
{(void)v;(void)p;(void)a;(void)b;return transport_error;}
static int mt7925_mlo_request_active_links(struct mt792x_dev *d,struct ieee80211_vif *v,u16 p)
{(void)d;(void)v;(void)p;return transport_error;}
static int ieee80211_request_neg_ttlm(struct ieee80211_vif *v,struct cfg80211_ttlm_params *p)
{(void)v;for(unsigned i=0;i<8;i++)if(p->dlink[i]!=3||p->ulink[i]!=3)return -EINVAL;return transport_error;}
'''
        cases = r'''
int main(void) {
 struct mt792x_dev d={0};struct ieee80211_vif v={.valid_links=7,.active_links=6,.type=2};
 struct mt792x_mlo_ticket t={0};mt7927_mlo_profiles_init(&v.p,true);
 if(mt7925_mlo_steer_request(&d,&v,5,NULL,&t)||v.p.pair[1].last_attempt_ns!=1) { puts("FAIL owned request attempt not recorded");return 1; }
 transport_error=-ECANCELED;
 if(mt7925_mlo_steer_request(&d,&v,5,NULL,&t)!=-ECANCELED||v.p.pair[1].last_error!=-ECANCELED)return 2;
 mt7925_test_mt7927_mlo_ttlm_removal=true;transport_error=-EIO;
 if(mt7925_mlo_request_active_links_negotiated(&d,&v,3)!=-EIO)return 3;
 if(!v.p.pair[0].last_attempt_ns||v.p.pair[0].last_error!=-EIO||strcmp(v.p.pair[0].last_reason,"negotiated_ttlm_request"))return 4;
 v.valid_links=6;
 if(mt7925_mlo_request_active_links_negotiated(&d,&v,3)!=-EINVAL||v.p.pair[0].last_error!=-EINVAL)return 5;
 v.valid_links=7;admission_error=-EACCES;
 if(mt7925_mlo_request_active_links_negotiated(&d,&v,3)!=-EACCES)return 6;
 if(strcmp(v.p.pair[0].last_reason,"pair_admission_refused"))return 7;
 mt7925_test_mt7927_mlo_ttlm_removal=false;transport_error=0;
 if(mt7925_mlo_steer_request(&d,&v,5,NULL,&t)!=-EACCES) { puts("FAIL tracked forward bypassed pair admission");return 8; }
 if(strcmp(v.p.pair[1].last_reason,"pair_admission_refused"))return 9;
 /* A ticketed rollback retains the security/epoch checks even when a user
  * disabled the former pair during an in-flight transition. */
 if(mt7925_mlo_steer_request(&d,&v,5,&t,&t))return 10;
 puts("request metadata: admission and ticketed recovery PASS");return 0;
}
'''
        ttlm_helper = function('mt7925/mlo/ttlm.c', 'mt7925_mlo_ttlm_request')
        bodies = '\n'.join(function('mt7925/mlo/steering.c', name) for name in (
            'mt7925_mlo_steer_request', 'mt7925_mlo_request_active_links_negotiated'))
        policy_h = (ROOT/'mt7925/mlo/policy.h').read_text()
        mapping_valid = policy_h[policy_h.index('static inline bool\nmt7925_mlo_ttlm_mapping_valid'):policy_h.index('#endif')]
        # the production reference to the TTLM API is weak: declare it exactly as the driver does
        weak_decl = re.search(r'extern int ieee80211_request_neg_ttlm\([^;]*\)\s*__weak;',
                              (ROOT/'mt7925/mlo/ttlm.c').read_text()).group(0)
        seams = seams.replace('static int ieee80211_request_neg_ttlm(',
                              weak_decl + '\nint ieee80211_request_neg_ttlm(', 1)
        prefix = '#define __weak __attribute__((weak))\n' + prefix
        bodies = mapping_valid + '\n' + ttlm_helper + '\n' + bodies
        with tempfile.TemporaryDirectory(prefix='mt7927-request-') as tmp:
            c, exe = Path(tmp)/'request.c', Path(tmp)/'request'
            c.write_text(prefix+declarations+policy+seams+bodies+cases)
            # -Wno-address: the fixture defines the weakly referenced API in the same unit
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-address',
                            '-Wno-sign-compare','-Wno-unused-function',str(c),'-o',str(exe)],check=True)
            result = subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


class PerDeviceDispatchTests(unittest.TestCase):
    def test_selection_is_owned_by_device_driver_not_global_hook(self):
        # Catch ignored device policy and cross-device global-hook overrides.
        body = function('tx.c', 'mt76_mlo_select_wcid')
        code = r'''
#include <stdio.h>
#include <stddef.h>
struct mt76_wcid { unsigned long flags; };
struct ieee80211_sta { int unused; };
struct sk_buff { int unused; };
struct mt76_dev;
struct mt76_driver_ops {
 struct mt76_wcid *(*tx_select_wcid)(struct mt76_dev *,struct mt76_wcid *,struct ieee80211_sta *,struct sk_buff *);
};
struct mt76_dev { unsigned chip; const struct mt76_driver_ops *drv; };
#define mt76_chip(d) ((d)->chip)
#define MT_WCID_FLAG_PS 0
#define test_bit(n,p) ((*(p)) & (1UL<<(n)))
static struct mt76_wcid *(*mt76_experimental_mlo_wcid_hook)(struct mt76_wcid *,struct ieee80211_sta *,struct sk_buff *);
static struct mt76_wcid selected;
static unsigned calls, foreign_calls;
static struct mt76_wcid *choose(struct mt76_dev *d,struct mt76_wcid *w,struct ieee80211_sta *s,struct sk_buff *b) {
 (void)d;(void)w;(void)s;(void)b;calls++;return &selected;
}
static struct mt76_wcid *foreign(struct mt76_wcid *w,struct ieee80211_sta *s,struct sk_buff *b) {
 (void)w;(void)s;(void)b;foreign_calls++;return &selected;
}
''' + body + r'''
int main(void) {
 struct mt76_driver_ops owned={.tx_select_wcid=choose}, plain={0};
 struct mt76_dev a={.chip=0x7927,.drv=&owned}, b={.chip=0x7927,.drv=&plain};
 struct mt76_wcid original={0};struct ieee80211_sta sta={0};struct sk_buff skb={0};
 unsigned failed=0;
#define CHECK(x) do {if(!(x)){printf("FAIL line%d: %s\n",__LINE__,#x);failed++;}}while(0)
 CHECK(mt76_mlo_select_wcid(&a,&original,&sta,&skb)==&selected);
 CHECK(calls==1);
 mt76_experimental_mlo_wcid_hook=foreign;
 CHECK(mt76_mlo_select_wcid(&b,&original,&sta,&skb)==&original);
 CHECK(foreign_calls==0);
 CHECK(mt76_mlo_select_wcid(&a,&original,NULL,&skb)==&original);
 CHECK(calls==1);
 selected.flags=1;
 CHECK(mt76_mlo_select_wcid(&a,&original,&sta,&skb)==&original);
 (void)mt76_experimental_mlo_wcid_hook;
 return failed!=0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source, binary = Path(directory) / 'dispatch.c', Path(directory) / 'dispatch'
            source.write_text(code)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', str(source),
                            '-o', str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class AssociationRequestAdmission(unittest.TestCase):
    def test_disabled_pair_cannot_mutate_roc(self):
        """Run actual association selection and real profile admission.

        Disabled explicit/automatic pairs must fail before firmware mutation;
        an enabled usable anchor-containing pair may be selected automatically.
        """
        h = (ROOT / 'mt7925/mlo/pairs.h').read_text()
        declarations = h[h.index('enum mt7927_mlo_support'):h.index('/* Kernel integration')]
        owner = function('mt7925/mlo/association.c', 'mt7925_mac_set_links')
        has_result = owner.startswith('static int')
        policy = '\n'.join(function('mt7925/mlo/pairs.c', name) for name in (
            'mt7927_mlo_pair_index', 'mt7927_mlo_profiles_init',
            'mt7927_mlo_profiles_admit'))
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
#define BIT(n) (1UL<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define READ_ONCE(x) (x)
#define hweight16(x) __builtin_popcount((unsigned)(x))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define for_each_set_bit(i,p,n) for((i)=0;(i)<(n);(i)++)if(*(p)&BIT(i))
#define __ffs(x) __builtin_ctzl((unsigned long)(x))
#define rcu_access_pointer(x) (x)
#define IEEE80211_MLD_MAX_NUM_LINKS 3
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
enum nl80211_band {NL80211_BAND_2GHZ,NL80211_BAND_5GHZ,NL80211_BAND_6GHZ};
struct ieee80211_channel {enum nl80211_band band;};
struct cfg80211_chan_def {struct ieee80211_channel *chan;};
struct ieee80211_bss_conf {struct {struct cfg80211_chan_def oper;}chanreq;};
struct mt792x_phy {int unused;};struct mt792x_bss_conf {int unused;};
struct mt792x_vif {struct mt792x_phy *phy;u8 deflink_id;struct mt792x_bss_conf bss_conf,*link_conf[3];
 struct {bool join_deferred;u16 join_deferred_links;u32 join_deferred_gen,transition_generation;}mt7927_mlo;};
struct ieee80211_vif {void *drv_priv;u16 valid_links,usable;bool mld;struct{bool assoc;}cfg;};
struct mt76_dev {bool chip;void *dev;};struct mt792x_dev {struct mt76_dev mt76;};
static bool mt7925_mlo_join_wait_bss;static u16 mt7925_test_mlo_pair_mask,selected=3;
static struct ieee80211_channel channels[3]={{0},{1},{2}};
static struct ieee80211_bss_conf confs[3];
static unsigned aborts,joins,requests,async_requests,primes,notes;static u16 requested,noted;
static int gate_error,last_error,join_error,request_error,abort_error,lock_depth;static u16 sev_decision;
static u16 mt7925_mlo_severity_decide(struct ieee80211_vif *v,unsigned long u){(void)v;(void)u;return sev_decision;}
static bool is_mt7927(struct mt76_dev *d){return d->chip;}
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v){return v->mld;}
static u16 ieee80211_vif_usable_links(struct ieee80211_vif *v){return v->usable;}
static u16 mt76_select_links(struct ieee80211_vif *v,unsigned n){(void)v;(void)n;return selected;}
static struct ieee80211_bss_conf *mt792x_vif_to_bss_conf(struct ieee80211_vif *v,unsigned id){(void)v;return &confs[id];}
static void mt792x_mutex_acquire(void *d){(void)d;lock_depth++;}
static void mt792x_mutex_release(void *d){(void)d;lock_depth--;}
static int mt7925_abort_roc(void *p,void *b){(void)p;(void)b;aborts++;return abort_error;}
static int join_fail_once;static bool ctx_ready=true;
static bool mt7927_mlo_link_ctx_installed(struct mt792x_dev *d,struct mt792x_vif *m,unsigned int id){(void)d;(void)m;(void)id;return ctx_ready;}
static int mt7925_set_mlo_roc(void *p,void *b,u16 target){(void)p;(void)b;joins++;requested=target;if(join_fail_once){join_fail_once=0;return -ENXIO;}return join_error;}
static int mt7925_mlo_request_active_links(void *d,void *v,u16 target){(void)d;(void)v;requests++;requested=target;return request_error;}
static void ieee80211_set_active_links_async(void *v,u16 target){(void)v;async_requests++;requested=target;}
static int mt7927_mlo_prime_dormant_link(void *d,void *v,void *m,unsigned id){(void)d;(void)v;(void)m;(void)id;primes++;return 0;}
'''
        boundary = r'''
static struct mt7927_mlo_profiles profiles;
static int mt7927_mlo_pairs_admit(struct ieee80211_vif *v,u16 target,bool negotiate)
{return gate_error ? gate_error : mt7927_mlo_profiles_admit(&profiles,target,v->valid_links,v->usable,v->cfg.assoc,negotiate);}
static int mt7927_mlo_pairs_prepare_association(struct ieee80211_vif *v,u16 target)
{return gate_error ? gate_error : mt7927_mlo_profiles_admit(&profiles,target,v->valid_links,v->usable,true,false);}
static bool mt7927_mlo_pair_enabled(struct ieee80211_vif *v,u16 target)
{(void)v;int idx=mt7927_mlo_pair_index(target);return idx>=0&&profiles.pair[idx].enabled;}
static void mt7927_mlo_pairs_note(struct ieee80211_vif *v,u16 target,int e,const char *why,bool done)
{(void)v;(void)why;(void)done;notes++;noted=target;last_error=e;}
static void reset(void){mt7927_mlo_profiles_init(&profiles,true);aborts=joins=requests=async_requests=primes=notes=0;
 gate_error=last_error=join_error=request_error=abort_error=0;mt7925_test_mlo_pair_mask=0;selected=3;sev_decision=0;join_fail_once=0;ctx_ready=true;mt7925_mlo_join_wait_bss=false;}
'''
        cases = r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL association admission line=%u abort=%u JOIN=%u request=%u prime=%u err=%d\n",__LINE__,aborts,joins,requests,primes,ret);return 1;}}while(0)
int main(void){struct mt792x_dev d={.mt76={.chip=true}};struct mt792x_phy phy={0};
 struct mt792x_vif m={.phy=&phy};struct ieee80211_vif v={.drv_priv=&m,.valid_links=7,.usable=7,.mld=true,.cfg={true}};int ret;
 for(unsigned id=0;id<3;id++)confs[id].chanreq.oper.chan=&channels[id];
 /* Ordinary and single-active-link association do not enter pair JOIN,
  * regardless of unavailable EMLSR or the valid7 autonomous-policy gate. */
 reset();v.mld=false;v.valid_links=v.usable=2;selected=2;CALL;
 CHECK(!ret&&!aborts&&!joins&&!requests&&!async_requests&&!primes);
 reset();v.mld=true;selected=2;CALL;
 CHECK(!ret&&!aborts&&!joins&&!requests&&!async_requests&&!primes);
 reset();v.valid_links=v.usable=4;selected=4;CALL;
 CHECK(!ret&&!aborts&&!joins&&!requests&&!async_requests&&!primes);
 v.valid_links=v.usable=7;
 reset();profiles.pair[0].enabled=false;profiles.pair[1].enabled=false;CALL;
 CHECK(!aborts&&!joins&&!requests&&!primes&&ret==-EACCES&&notes==1&&last_error==-EACCES);
 reset();mt7925_test_mlo_pair_mask=5;profiles.pair[1].enabled=false;CALL;
 CHECK(!aborts&&!joins&&!requests&&!primes&&ret==-EACCES&&noted==5);
 reset();profiles.pair[0].enabled=false;CALL;
 CHECK(!ret&&aborts==1&&joins==1&&requests==1&&requested==5&&primes==1&&!lock_depth);
 reset();v.usable=3;profiles.pair[0].enabled=false;CALL;
 CHECK(ret==-EACCES&&!aborts&&!joins&&!requests);v.usable=7;
 reset();join_error=-ETIMEDOUT;CALL;CHECK(ret==-ETIMEDOUT&&aborts==1&&joins==1&&!requests&&!primes&&last_error==-ETIMEDOUT);
 reset();request_error=-ESTALE;CALL;CHECK(ret==-ESTALE&&requests==1&&!primes);
 reset();gate_error=-ENOTCONN;CALL;CHECK(ret==-ENOTCONN&&!aborts&&!joins&&!requests);
 reset();abort_error=-EIO;CALL;CHECK(ret==-EIO&&aborts==1&&!joins&&!requests);
 reset();v.cfg.assoc=false;CALL;
 CHECK(!ret&&aborts==1&&joins==1&&!requests&&async_requests==1&&requested==3);
 reset();profiles.pair[0].enabled=false;profiles.pair[1].enabled=false;CALL;
 CHECK(ret==-EACCES&&!aborts&&!joins&&!requests&&!async_requests);
 /* Second caller: the previous connection is severe, so the pair it decides on
  * replaces the automatic one, but only a real pair that keeps the association
  * anchor, and never over an explicit diagnostic pair. */
 reset();m.deflink_id=0;sev_decision=5;CALL;
 CHECK(!ret&&joins==1&&requested==5);
 reset();m.deflink_id=1;sev_decision=0;CALL;
 {unsigned bj=joins,bp=primes,br=requests;u16 bq=requested;
  reset();m.deflink_id=1;sev_decision=5;CALL; /* 0x5 excludes anchor link1: no change at all */
  CHECK(joins==bj&&primes==bp&&requests==br&&requested==bq);}
 reset();m.deflink_id=0;sev_decision=0;CALL;
 CHECK(!ret&&joins==1&&requested==3); /* nothing severe: no override */
 reset();m.deflink_id=0;mt7925_test_mlo_pair_mask=3;sev_decision=5;CALL;
 CHECK(!ret&&joins==1&&requested==3); /* explicit test pair wins */
 reset();m.deflink_id=0;sev_decision=2;CALL;
 CHECK(!ret&&joins==1&&requested==3); /* a single link is not a pair override */
 /* Fallback to the original (automatic) pair when the second caller's pair does not come up */
 reset();m.deflink_id=0;sev_decision=5;join_fail_once=1;CALL;
 CHECK(!ret&&joins==2&&requested==3); /* override failed once, automatic pair succeeded */
 reset();m.deflink_id=0;sev_decision=5;join_error=-ETIMEDOUT;CALL;
 CHECK(ret==-ETIMEDOUT&&joins==2); /* both fail: one fallback only, no loop */
 reset();m.deflink_id=0;sev_decision=0;join_fail_once=1;CALL;
 CHECK(ret==-ENXIO&&joins==1); /* no override taken: no fallback attempt */
 /* Deferred JOIN: when the partner's BSS context is not installed, the JOIN must not be
  * built at association time even if a stale partner config pointer exists (a link that
  * an earlier shrink tore down). Hardware: the builder rejected it with -ENXIO and the
  * association failed. The default (deferral off) is unchanged. */
 reset();m.deflink_id=0;m.mt7927_mlo.join_deferred=false;m.link_conf[1]=(struct mt792x_bss_conf *)&m.bss_conf;
 mt7925_mlo_join_wait_bss=true;ctx_ready=false;CALL;
 CHECK(!ret&&joins==0&&m.mt7927_mlo.join_deferred); /* stale pointer, no context: defer */
 reset();m.deflink_id=0;m.mt7927_mlo.join_deferred=false;m.link_conf[1]=(struct mt792x_bss_conf *)&m.bss_conf;
 mt7925_mlo_join_wait_bss=true;ctx_ready=true;CALL;
 CHECK(!ret&&joins==1&&!m.mt7927_mlo.join_deferred); /* context installed: JOIN now */
 reset();m.deflink_id=0;m.mt7927_mlo.join_deferred=false;m.link_conf[1]=NULL;
 mt7925_mlo_join_wait_bss=true;ctx_ready=false;CALL;
 CHECK(!ret&&joins==0&&m.mt7927_mlo.join_deferred); /* no partner at all: defer as before */
 reset();m.deflink_id=0;m.mt7927_mlo.join_deferred=false;m.link_conf[1]=(struct mt792x_bss_conf *)&m.bss_conf;
 mt7925_mlo_join_wait_bss=false;ctx_ready=false;CALL;
 CHECK(!ret&&joins==1&&!m.mt7927_mlo.join_deferred); /* deferral off: unchanged */
 m.link_conf[1]=NULL;
 m.deflink_id=0;
 CHECK(mt7927_mlo_pairs_admit(&v,3,false)==-ENOTCONN);
 puts("association admission: 25 cases PASS");return 0;}
'''.replace('CALL', 'ret=mt7925_mac_set_links(&d.mt76,&v)' if has_result else
              'mt7925_mac_set_links(&d.mt76,&v);ret=0')
        with tempfile.TemporaryDirectory(prefix='mt7927-assoc-admit-') as directory:
            c, exe = Path(directory) / 'test.c', Path(directory) / 'test'
            c.write_text(prefix + declarations + policy + boundary + owner + cases)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', str(c), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class SteeringUnknownScoring(unittest.TestCase):
    def test_validation_invalidates_lost_membership_and_can_restore_singleton(self):
        body = function('mt7925/mlo/steering.c', 'mt7925_mlo_steer_switch_validate')
        start = body.index('\tif (mvif->mlo_steer_rollback_pending)')
        end = body.index('\n\t/* A rollback restores')
        # Include any new membership guard immediately preceding rollback handling.
        earlier = body.rfind('\tif (vif->valid_links', 0, start)
        guard = body[earlier if earlier >= 0 else start:end]
        expr = re.search(r'prev_is_mode = (.*?);', body, re.S).group(1)
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define hweight16(x) __builtin_popcount((unsigned)(x))
#define __ffs(x) __builtin_ctz((unsigned)(x))
enum { NL80211_BAND_2GHZ, NL80211_BAND_5GHZ, NL80211_BAND_6GHZ };
struct ieee80211_vif { unsigned valid_links; };
struct mvif { bool mlo_steer_rollback_pending; };
struct mt7925_mlo_mode_ctx { unsigned unused; };
static unsigned mt7925_mlo_mode_of_links(unsigned p,struct mt7925_mlo_mode_ctx*c){(void)c;return p==3||p==5;}
static unsigned __attribute__((unused)) mt7925_mlo_link_band(struct ieee80211_vif*v,unsigned id){(void)v;return id;}
static int entry(struct ieee80211_vif*vif,struct mvif*mvif){
 (void)vif;
'''
        middle = r'''
 return 0;
rollback_wait:return 1;
invalidated: __attribute__((unused)); return 2;
}
static bool restore(struct ieee80211_vif*vif,unsigned prev_pair){
 struct mt7925_mlo_mode_ctx mctx={0};bool prev_is_mode;(void)vif;
'''
        cases = r'''
 return prev_is_mode;
}
int main(void){struct ieee80211_vif v={3};struct mvif m={false};
 if(entry(&v,&m)!=2)return 1;
 v.valid_links=7;if(entry(&v,&m))return 2;
 if(!restore(&v,2)||!restore(&v,4)||!restore(&v,3)||!restore(&v,5))return 3;
 if(restore(&v,6)||restore(&v,1))return 4;
 return 0;}
'''
        with tempfile.TemporaryDirectory(prefix='mt7927-validation-membership-') as tmp:
            c, exe = Path(tmp)/'validation.c', Path(tmp)/'validation'
            c.write_text(prefix + guard + middle + 'prev_is_mode = ' + expr + ';' + cases)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_secure_singleton_fallback_is_not_an_emlsr_mode(self):
        source = (ROOT/'mt7925/mlo/steering.c').read_text()
        worker = function('mt7925/mlo/steering.c', 'mt7925_mlo_steer_work')
        selection = worker[worker.index('\tselected_pair = 0;'):worker.index('\n\tif (selected_pair) {')]
        helper = (function('mt7925/mlo/steering.c', 'mt7925_mlo_singleton_fallback')
                  if 'static u16 mt7925_mlo_singleton_fallback(' in source else '')
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define S32_MIN INT32_MIN
#define READ_ONCE(x) (x)
#define MT7925_MLO_STEER_HYSTERESIS_DB 5
#define MT7925_MLO_STEER_COOLDOWN 20000
#define time_after(a,b) ((long)((b)-(a)) < 0)
enum { MT7925_MLO_BAND_2G, MT7925_MLO_BAND_5G, MT7925_MLO_BAND_6G };
struct mt7925_mlo_mode_ctx { unsigned band_present,band_link[3]; };
struct mvif { unsigned long mlo_last_switch_jiffies; unsigned deflink_id; struct { unsigned verified_switch_classes; } mt7927_mlo; };
static int mt7925_mlo_steer_try_unverified __attribute__((unused));
static unsigned __attribute__((unused)) mt7925_mlo_switch_class(unsigned a,unsigned b,int*c){(void)a;(void)b;(void)c;return 1;}
static bool __attribute__((unused)) mt7925_mlo_switch_feasible(unsigned a,unsigned b,bool c){(void)a;return b||c;}
static unsigned severe_links;
static bool mt7925_mlo_severity_blocked(void*v,unsigned l,const int*b){(void)v;(void)b;return severe_links&&l==severe_links;}
'''
        cases = r'''
static unsigned run_a(unsigned anchor,unsigned current_links,unsigned secure_now,int cur_score,int best_score,int cur_degrade_penalty,unsigned best_pair,bool verified) {
 struct mvif m={.deflink_id=anchor,.mt7927_mlo={verified}},*mvif=&m;
 struct mt7925_mlo_mode_ctx mctx __attribute__((unused))={7,{0,1,2}};
 int band_of_link[3] __attribute__((unused))={0,1,2};
 unsigned best_experimental_pair=0,selected_pair;
 bool cur_is_mode=true;unsigned long jiffies=30000;const char*selected_reason;void*vif=0;
 (void)secure_now;(void)vif;
'''
        suffix = r'''
 (void)selected_reason;return selected_pair;
}
static unsigned run(unsigned current_links,unsigned secure_now,int cur_score,int best_score,int cur_degrade_penalty,unsigned best_pair,bool verified) {
 return run_a(current_links==5?2:1,current_links,secure_now,cur_score,best_score,cur_degrade_penalty,best_pair,verified);
}
int main(void) {
 if(run(5,4,-80,S32_MIN,30,5,true)!=4){puts("missing secure 6GHz singleton fallback");return 1;}
 if(run(3,2,S32_MIN,S32_MIN,0,3,true)!=2)return 2;
 if(run(5,1,-80,S32_MIN,30,5,true))return 3; /* high-band not secure */
 if(run(5,4,-80,S32_MIN,0,5,true))return 4; /* healthy current pair */
 if(run(5,4,-80,-60,30,3,true)!=3)return 5; /* healthy eligible pair wins */
 if(run(5,4,-80,S32_MIN,30,5,false))return 6; /* no unverified class bypass */
 if(run(5,4,-80,-80,30,5,true)!=4)return 7; /* degraded current pair is not a healthy alternative */
 severe_links=4; if(run(5,4,-80,S32_MIN,30,5,true))return 8; /* a severe single-link class is not the fallback */ severe_links=0;
 if(run(5,4,-80,S32_MIN,30,5,true)!=4)return 9; /* severity cleared: fallback again */
 /* the fallback keeps the association anchor: a 2.4 GHz anchor (link 0) is never given up for a
  * single high-band link, the anchor-excluded class the firmware cannot carry */
 if(run_a(0,5,4,-80,S32_MIN,30,5,true))return 10;
 if(run_a(0,3,2,S32_MIN,S32_MIN,0,3,true))return 11;
 if(run_a(2,5,4,-80,S32_MIN,30,5,true)!=4)return 12; /* 6 GHz anchor keeps the 6 GHz single */
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='mt7927-singleton-fallback-') as tmp:
            c, exe = Path(tmp)/'fallback.c', Path(tmp)/'fallback'
            c.write_text(prefix + helper + cases + selection + suffix)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ttlm_request_api_absent_in_mac80211_is_not_fatal(self):
        """The TTLM request API exists only in a patched mac80211. A driver built for and
        loaded on a stock mac80211 must still load: the reference is weak and the
        request reports -EOPNOTSUPP instead of calling a missing symbol."""
        source = (ROOT/'mt7925/mlo/ttlm.c').read_text()
        decl = re.search(r'extern int ieee80211_request_neg_ttlm\([^;]*\)\s*__weak;', source)
        self.assertIsNotNone(decl, 'weak declaration of ieee80211_request_neg_ttlm missing')
        helper = function('mt7925/mlo/ttlm.c', 'mt7925_mlo_ttlm_request')
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint16_t u16;
#define __weak __attribute__((weak))
#define IEEE80211_TTLM_NUM_TIDS 8
struct ieee80211_vif { u16 valid_links; };
struct cfg80211_ttlm_params { u16 dlink[8],ulink[8]; };
static bool mt7925_mlo_ttlm_mapping_valid(u16 v,const u16 *d,const u16 *u){(void)v;(void)d;(void)u;return true;}
'''
        absent = prefix + decl.group(0) + '\n' + helper + r'''
int main(void){struct ieee80211_vif v={.valid_links=7};
 if(ieee80211_request_neg_ttlm){puts("symbol should be absent");return 1;}
 if(mt7925_mlo_ttlm_request(&v,3)!=-EOPNOTSUPP){puts("absent API is not reported");return 2;}
 return 0;}
'''
        present = prefix + decl.group(0) + '\n' + helper + r'''
int ieee80211_request_neg_ttlm(struct ieee80211_vif *v,struct cfg80211_ttlm_params *p){(void)v;(void)p;return 0;}
int main(void){struct ieee80211_vif v={.valid_links=7};
 if(mt7925_mlo_ttlm_request(&v,3)!=0){puts("present API not called");return 1;}
 return 0;}
'''
        for name, text in (('absent', absent), ('present', present)):
            with tempfile.TemporaryDirectory(prefix='mt7927-ttlm-weak-') as tmp:
                c, exe = Path(tmp)/f'{name}.c', Path(tmp)/name
                c.write_text(text)
                subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                str(c), '-o', str(exe)], check=True)
                result = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, name + ': ' + result.stdout + result.stderr)

    def test_worker_requires_real_three_band_membership_before_policy(self):
        body = function('mt7925/mlo/steering.c', 'mt7925_mlo_steer_work')
        body = body[body.index('\tif (!READ_ONCE'):body.index('\tmt7925_mlo_rotate_rssi_monitor')]
        body = body.replace('goto requeue;', 'return;')
        prefix = r'''
#include <stdbool.h>
#include <stdio.h>
#define READ_ONCE(x) (x)
#define BIT(n) (1U << (n))
#define WRITE_ONCE(x,v) ((x)=(v))
struct ieee80211_vif { unsigned valid_links; bool mld; struct { bool assoc; } cfg; };
struct mt792x_vif { struct { bool enabled; } mlo_completion;
 bool mlo_steer_validating,mlo_auto_steering; unsigned mlo_steer_prev_pair,mlo_steer_pending_pair,mlo_steer_ticket;
 struct { unsigned negotiated_links; } mt7927_mlo; };
struct mt792x_dev { int unused; };
struct mt7925_mlo_mode_ctx { unsigned band_present; };
enum { MT7925_MLO_BAND_2G, MT7925_MLO_BAND_5G, MT7925_MLO_BAND_6G };
static unsigned bands=7,reached;
static bool mt7927_mlo_pairs_can_steer(struct ieee80211_vif *v) {(void)v;return true;}
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v) {return v->mld;}
static void mt7925_mlo_steer_switch_validate(struct mt792x_dev*d,struct ieee80211_vif*v,struct mt792x_vif*m,unsigned p,unsigned n,unsigned*t)
{(void)d;(void)v;(void)m;(void)p;(void)n;(void)t;}
static void __attribute__((unused)) mt7925_mlo_build_mode_ctx(struct ieee80211_vif*v,struct mt792x_vif*m,struct mt7925_mlo_mode_ctx*c)
{(void)v;(void)m;c->band_present=bands;}
static void run(struct ieee80211_vif*vif,struct mt792x_vif*mvif,struct mt792x_dev*dev) {
 struct mt7925_mlo_mode_ctx mctx __attribute__((unused));
'''
        cases = r'''
 reached++;
}
int main(void) {
 struct ieee80211_vif v={.valid_links=3,.mld=true,.cfg={true}};
 struct mt792x_vif m={.mlo_completion={true},.mlo_auto_steering=true,.mt7927_mlo={3}};
 struct mt792x_dev d={0};
 run(&v,&m,&d);if(reached){puts("partial membership reached policy");return 1;}
 v.valid_links=7;run(&v,&m,&d);if(reached)return 2;
 m.mt7927_mlo.negotiated_links=7;bands=3;run(&v,&m,&d);if(reached)return 3;
 bands=7;run(&v,&m,&d);if(reached!=1)return 4;
 /* An active singleton is not excluded: this guard is membership, not active state. */
 run(&v,&m,&d);if(reached!=2)return 5;
 v.mld=false;run(&v,&m,&d);if(reached!=2)return 6;
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='mt7927-worker-membership-') as tmp:
            c, exe = Path(tmp)/'worker.c', Path(tmp)/'worker'
            c.write_text(prefix + body + cases)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    """Catch invented dormant RSSI and missing UNKNOWN experimental marking."""

    def test_real_scorer_preserves_unknown_metrics_and_degradation(self):
        source = (ROOT / 'mt7925/mlo/steering.c').read_text()
        constants = '\n'.join(line for line in source.splitlines()
                              if re.match(r'#define MT7925_MLO_(RSSI_STALE_JIFFIES|'
                                          r'STEER_(UNKNOWN_SCORE|STALE_PENALTY_PER_SEC|'
                                          r'STALE_PENALTY_MAX|6G_DEGRADE_THRESHOLD|'
                                          r'6G_DEGRADE_PENALTY|5G6G_BONUS))\s', line))
        enum = source[source.index('enum mt7925_mlo_link_metric_state {'):]
        enum = enum[:enum.index('};') + 2]
        metric = function('mt7925/mlo/steering.c', 'mt7925_mlo_link_metric')
        passive = function('mt7925/mlo/steering.c', 'mt7925_mlo_passive_rssi')
        scorer = function('mt7925/mlo/steering.c', 'mt7925_mlo_pair_score_parts')
        bonus = function('mt7925/mlo/steering.c', 'mt7925_mlo_pair_bonus')
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
#define HZ 1000
#define BIT(n) (1UL << (n))
#define S32_MIN INT32_MIN
#define IEEE80211_MLD_MAX_NUM_LINKS 3
#define MT792X_MLO_RSSI_UNKNOWN (-128)
#define READ_ONCE(x) (x)
#define time_after(a,b) ((long)((b)-(a)) < 0)
#define time_before(a,b) time_after(b,a)
#define jiffies_to_msecs(x) (x)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define for_each_set_bit(i,p,n) for ((i)=0;(i)<(n);(i)++) if (*(p)&BIT(i))
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define printk(...) ((void)0)
enum nl80211_band { NL80211_BAND_2GHZ, NL80211_BAND_5GHZ, NL80211_BAND_6GHZ };
struct mt792x_link_sta { struct { unsigned hw_key_idx; u32 rate; bool link_valid; unsigned rssi; } wcid; unsigned long mlo_valid_rx_jiffies; };
#define ewma_signal_read(p) (*(p))
struct mt792x_sta { struct mt792x_link_sta *links[3]; };
struct ieee80211_sta { struct mt792x_sta drv_priv[1]; };
struct mt792x_vif {
 struct { unsigned negotiated_links, resident_links; } mt7927_mlo;
 s32 mlo_link_rssi[3]; unsigned long mlo_link_rssi_jiffies[3];
 unsigned long mlo_steer_link_cooldown[3];
};
struct ieee80211_vif { struct mt792x_vif drv_priv[1]; struct { int ap_addr; } cfg; };
static unsigned long jiffies=100000;
static int mt7925_mlo_link2_rssi_override, mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
static struct ieee80211_sta sta;
static struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *v,int a)
{ (void)v; (void)a; return &sta; }
static struct mt792x_link_sta *mt792x_sta_link_rcu(struct mt792x_sta *s,int i)
{ return s ? s->links[i] : NULL; }
static u32 cfg80211_calculate_bitrate(u32 *r) { return *r; }
static enum nl80211_band mt7925_mlo_link_band(struct ieee80211_vif *v,int i)
{ (void)v; return (enum nl80211_band)i; }
'''
        cases = r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
 struct ieee80211_vif v={0}; struct mt792x_vif *m=v.drv_priv;
 struct mt792x_link_sta l0={.wcid.hw_key_idx=1}, l1={.wcid.hw_key_idx=1}, l2={.wcid.hw_key_idx=1};
 s32 score,rssi,bonus,anchor,penalty; bool experimental;
 m->mt7927_mlo.negotiated_links=7; m->mt7927_mlo.resident_links=6;
 m->mlo_link_rssi[0]=-128; m->mlo_link_rssi[1]=-40; m->mlo_link_rssi[2]=-80;
 m->mlo_link_rssi_jiffies[1]=m->mlo_link_rssi_jiffies[2]=jiffies;
 sta.drv_priv->links[1]=&l1; sta.drv_priv->links[2]=&l2;
 /* Passive per-link signal: only an ACTIVE link whose security-valid RX is
  * recent and whose WCID averaged a real signal is LIVE from it. */
 { s32 sc; struct mt792x_link_sta *l1p=&l1;
   l1.wcid.link_valid=true; l1.wcid.rssi=45; l1.mlo_valid_rx_jiffies=jiffies;
   CHECK(mt7925_mlo_link_metric(m,6,1,l1p,&sc)==MT7925_MLO_METRIC_LIVE && sc==-45);
   CHECK(mt7925_mlo_link_metric(m,4,1,l1p,&sc)==MT7925_MLO_METRIC_CACHED); /* dormant: not passive */
   l1.mlo_valid_rx_jiffies=jiffies-31000; /* RX stopped: stale reading is not trusted */
   CHECK(mt7925_mlo_link_metric(m,6,1,l1p,&sc)==MT7925_MLO_METRIC_LIVE && sc==-40); /* hw sample */
   l1.mlo_valid_rx_jiffies=0;
   CHECK(mt7925_mlo_link_metric(m,6,1,l1p,&sc)==MT7925_MLO_METRIC_LIVE && sc==-40);
   l1.wcid.rssi=0; l1.mlo_valid_rx_jiffies=jiffies; /* never averaged: 0 is not a signal */
   CHECK(mt7925_mlo_link_metric(m,6,1,l1p,&sc)==MT7925_MLO_METRIC_LIVE && sc==-40);
   l1.wcid.link_valid=false; l1.wcid.rssi=45;
   CHECK(mt7925_mlo_link_metric(m,6,1,l1p,&sc)==MT7925_MLO_METRIC_LIVE && sc==-40);
   l1.wcid.link_valid=false; l1.wcid.rssi=0; l1.mlo_valid_rx_jiffies=0; }
 /* Negotiated but never sampled/admitted L0: no anchor-derived fake RSSI. */
 score=mt7925_mlo_pair_score_parts(&v,6,5,&rssi,&bonus,&anchor,&penalty,&experimental);
 CHECK(score==-150 && rssi==-150 && bonus==0 && anchor==-80 && penalty==0 && experimental);
 /* Real 6G degradation affects the current pair only. */
 score=mt7925_mlo_pair_score_parts(&v,6,6,&rssi,&bonus,&anchor,&penalty,&experimental);
 CHECK(score==-142 && rssi==-150 && bonus==8 && penalty==30 && !experimental);
 /* The 6 GHz anchor degrades ANY current pair that rides on it, not only the
  * old 5+6 GHz mask: the 2.4+6 mask (0x5) is penalised when current, and no
  * candidate is penalised for an anchor it is not currently using. */
 score=mt7925_mlo_pair_score_parts(&v,5,5,&rssi,&bonus,&anchor,&penalty,&experimental);
 CHECK(penalty==30 && rssi==-180 && anchor==-80);
 score=mt7925_mlo_pair_score_parts(&v,5,3,&rssi,&bonus,&anchor,&penalty,&experimental);
 CHECK(penalty==0);
 /* No anchor in the current pair: nothing to degrade. */
 score=mt7925_mlo_pair_score_parts(&v,3,5,&rssi,&bonus,&anchor,&penalty,&experimental);
 CHECK(penalty==0);
 /* A genuine dormant measurement is cached, not experimental. */
 m->mlo_link_rssi[0]=-50; m->mlo_link_rssi_jiffies[0]=jiffies;
 sta.drv_priv->links[0]=&l0;
 score=mt7925_mlo_pair_score_parts(&v,6,5,&rssi,&bonus,&anchor,&penalty,&experimental);
 CHECK(score==-130 && !experimental);
 /* Cached RSSI without key material is ineligible. */
 l0.wcid.hw_key_idx=255;
 CHECK(mt7925_mlo_pair_score_parts(&v,6,5,&rssi,&bonus,&anchor,&penalty,&experimental)==S32_MIN);
 /* Missing negotiated membership is never an exploratory admission. */
 m->mt7927_mlo.negotiated_links=6; m->mlo_link_rssi[0]=-128;
 CHECK(mt7925_mlo_pair_score_parts(&v,6,5,&rssi,&bonus,&anchor,&penalty,&experimental)==S32_MIN);
 return 0;
}
'''
        missing_unknown = re.sub(
            r'\n\tif \(mvif->mlo_link_rssi\[link_id\] == MT792X_MLO_RSSI_UNKNOWN\) \{'
            r'\n\t\t\*out_score = MT7925_MLO_STEER_UNKNOWN_SCORE;'
            r'\n\t\treturn MT7925_MLO_METRIC_UNKNOWN;\n\t\}', '', metric, count=1)
        self.assertNotEqual(metric, missing_unknown)
        with tempfile.TemporaryDirectory(prefix='mt7927-unknown-score-') as tmp:
            c, exe = Path(tmp) / 'score.c', Path(tmp) / 'score'
            for label, body, expected in (('actual', metric, 0),
                                          ('missing_unknown_return', missing_unknown, 1)):
                c.write_text(prefix + constants + '\n' + enum + '\n' + passive + '\n' + body + '\n' + bonus + '\n' + scorer + '\n' + cases)
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                str(c), '-o', str(exe)], check=True)
                result = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, label + result.stdout + result.stderr)
                if expected:
                    self.assertIn('score==-150', result.stderr)


class RadioAbortOwnership(unittest.TestCase):
    def test_failed_abort_retains_owner_and_reports_completion_error(self):
        # The real release coordinator must not forget a grant before its
        # existing MCU dependency succeeds. These are command results, not
        # simulated firmware RF-release acknowledgments.
        prefix = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
typedef unsigned char u8;
#include "mt7925/mlo_roc_state.h"
#define IEEE80211_MLD_MAX_NUM_LINKS 3
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define BIT(i) (1UL<<(i))
#define READ_ONCE(x) (x)
#define KERN_INFO ""
#define printk(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define for_each_set_bit(i,p,n) for ((i)=0;(i)<(n);(i)++) if (*(p)&BIT(i))
#define __clear_bit(i,p) (*(p)&=~BIT(i))
struct mt76_dev { bool dut; };
struct mt792x_dev { struct mt76_dev mt76; };
struct mt792x_phy { struct mt792x_dev *dev; struct mt7925_mlo_roc_state mlo_roc; };
struct mt792x_bss_conf { struct { unsigned idx; } mt76; };
struct mt792x_vif { struct mt792x_bss_conf *links[3]; };
struct ieee80211_vif { struct mt792x_vif drv_priv[1]; };
#define is_mt7927(d) ((d)->dut)
#define mt792x_vif_to_link(v,id) ((v)->links[id])
static int mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
static unsigned rejected, failed, calls, retained_at_send, seen_tokens[3];
static int completion_error;
static bool __mt7925_mlo_roc_abort_guard(struct mt792x_phy *p,
 struct mt792x_vif *v, unsigned token,unsigned link,unsigned bss,
 bool teardown,const char *reason)
{ (void)p;(void)v;(void)token;(void)bss;(void)teardown;(void)reason;
 return !(rejected&BIT(link)); }
static int mt7925_mcu_abort_mlo_roc(struct mt792x_phy *p,
 struct mt792x_bss_conf *b,u8 token)
{ calls|=BIT(b->mt76.idx); seen_tokens[b->mt76.idx]=token;
 retained_at_send|=p->mlo_roc.held_links&BIT(b->mt76.idx);
 return (failed&BIT(b->mt76.idx))?-EIO:0; }
static void mt7925_mlo_completion_note_error(struct ieee80211_vif *v,int err)
{ (void)v;if(!completion_error) completion_error=err; }
static void mt7925_mlo_roc_trace(struct mt792x_phy *p,const char *reason)
{ (void)p;(void)reason; }
'''
        cases = r'''
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"case=%d: %s\n",mode,#x);return 1;} } while(0)
int main(void) {
 for(int mode=0;mode<6;mode++) {
  struct mt792x_dev d={.mt76={.dut=mode!=5}};
  struct mt792x_phy p={.dev=&d};struct ieee80211_vif v={0};
  struct mt792x_bss_conf b[3]={{{0}},{{1}},{{2}}};
  for(unsigned i=0;i<3;i++) {v.drv_priv->links[i]=&b[i];p.mlo_roc.link_token[i]=10+i;}
  p.mlo_roc.held_links=7;p.mlo_roc.held_token=10;
  rejected=mode==3?2:0;failed=(mode==1||mode==4||mode==5)?2:0;
  if(mode==2) v.drv_priv->links[1]=NULL;
  calls=retained_at_send=0;completion_error=0;
  unsigned mask=mode==4?2:7;
  mt7925_mlo_roc_release_held(&p,v.drv_priv,mask,false,"fixture");
  if(mode==5) {CHECK(p.mlo_roc.held_links==0);CHECK(completion_error==0);continue;}
  CHECK(p.mlo_roc.held_links==(mode==0?0:mode==4?7:2));
  CHECK(completion_error==((mode==1||mode==4)?-EIO:mode==2?-ENXIO:0));
  CHECK(calls==(mode==4?2:(mode==2||mode==3)?5:7));
  if(mode!=0) CHECK(retained_at_send==calls);
  for(unsigned i=0;i<3;i++) if(calls&BIT(i)) CHECK(seen_tokens[i]==10+i);
  if(p.mlo_roc.held_links&2) CHECK(p.mlo_roc.link_token[1]==11);
 }
 return 0;
}
'''
        source = prefix + '\n' + function('mt7925/mlo/radio.c', 'mt7925_abort_mlo_roc_slots') + '\n' + function('mt7925/mlo/radio.c', 'mt7925_mlo_roc_release_held') + cases
        with tempfile.TemporaryDirectory(prefix='mt7927-abort-owner-') as tmp:
            path, executable = Path(tmp)/'release.c', Path(tmp)/'release'
            path.write_text(source)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra',
                            '-Wno-unused-function', '-Werror', '-I', str(ROOT),
                            str(path), '-o', str(executable)], check=True)
            result = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
