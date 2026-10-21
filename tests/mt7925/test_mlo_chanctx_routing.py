#!/usr/bin/env python3
"""Compile the actual chanctx callback, recording its MCU boundary arguments.

Catches a channel update sent to an unrelated survivor BSS after a no-match
lookup, and a resident incoming link omitted by the driver's stale mask.
This verifies callback routing; it does not simulate mac80211 or firmware.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

def run_fixture(root=None):
    root = Path(root) if root is not None else Path(__file__).resolve().parents[2]
    owner = root / 'mt7925/mlo/association.c'
    if not owner.exists():
     print('FAIL channel execution has no association owner', file=sys.stderr)
     raise AssertionError('channel execution has no association owner')
    source = owner.read_text()
    start = source.index('void\nmt7927_mlo_assoc_change_chanctx(')
    end = source.index('\n}', start) + 2
    function = source[start:end]
    prefix = r'''
    #include <stdbool.h>
    #include <stdint.h>
    #include <stddef.h>
    #include <stdio.h>
    #include <string.h>
    #include <stdarg.h>
    typedef uint8_t u8;
    typedef uint32_t u32;
    #define IEEE80211_MLD_MAX_NUM_LINKS 3
    #define NL80211_IFTYPE_MONITOR 6
    #define IEEE80211_CHANCTX_CHANGE_PUNCTURING 1
    #define READ_ONCE(x) (x)
    #define KERN_INFO ""
    #define container_of(p, t, m) ((t *)((char *)(p)-offsetof(t,m)))
    #define for_each_set_bit(i, p, n) for ((i)=0; (i)<(n); (i)++) if ((*(p)) & (1UL<<(i)))
    static bool mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
    struct mt792x_bss_conf;
    struct mt792x_vif;
    struct mt792x_chanctx { struct mt792x_bss_conf *bss_conf; };
    struct channel { unsigned hw_value; };
    struct ieee80211_chanctx_conf {
     struct { struct channel *chan; } def;
     struct mt792x_chanctx drv_priv[1];
    };
    struct mt76_phy { int unused; };
    struct mt792x_dev { int unused; };
    struct mt792x_phy { struct mt792x_dev *dev; struct mt76_phy *mt76; };
    struct ieee80211_bss_conf { unsigned link_id; };
    struct mt792x_bss_conf {
     struct { struct ieee80211_chanctx_conf *ctx; unsigned idx; } mt76;
     struct mt792x_vif *vif;
     unsigned link_id;
    };
    struct mt792x_vif {
     unsigned valid_links;
     struct mt792x_phy *phy;
     struct mt792x_bss_conf bss_conf, *link_conf[3];
    };
    struct ieee80211_vif { unsigned valid_links; int type; bool mld; struct mt792x_vif drv_priv; };
    struct ieee80211_hw { struct mt792x_phy *phy; };
    static struct ieee80211_bss_conf bss_confs[3];
    static unsigned calls, target, channel, puncture_calls, locks, invalidations;
    static void mt7927_mlo_pairs_invalidate(struct ieee80211_vif *vif, bool security)
    { (void)vif; (void)security; invalidations++; }
    static struct mt792x_phy *mt792x_hw_phy(struct ieee80211_hw *hw) { return hw->phy; }
    static bool ieee80211_vif_is_mld(struct ieee80211_vif *vif) { return vif->mld; }
    static struct mt792x_bss_conf *mt792x_vif_to_link(struct mt792x_vif *v, unsigned id)
    { return v->link_conf[id] ? v->link_conf[id] : &v->bss_conf; }
    static struct ieee80211_bss_conf *mt792x_vif_to_bss_conf(struct ieee80211_vif *v, unsigned id)
    { (void)v; return &bss_confs[id]; }
    static void mt792x_mutex_acquire(struct mt792x_dev *d) { (void)d; locks++; }
    static void mt792x_mutex_release(struct mt792x_dev *d) { (void)d; locks--; }
    static void mt7925_monitor_arm_sniffer(struct mt792x_phy *p, struct ieee80211_vif *v, struct ieee80211_chanctx_conf *c)
    { (void)p; (void)v; (void)c; }
    static void mt76_phy_chandef_set(void *p, void *d) { (void)p; (void)d; }
    static void mt7925_mcu_set_chctx(struct mt76_phy *p, void *m, struct ieee80211_bss_conf *b, struct ieee80211_chanctx_conf *c)
    { (void)p; (void)m; calls++; target=b->link_id; channel=c->def.chan->hw_value; }
    static void mt7925_mcu_set_eht_pp(struct mt76_phy *p, void *m, struct ieee80211_bss_conf *b, struct ieee80211_chanctx_conf *c)
    { (void)p; (void)m; (void)b; (void)c; puncture_calls++; }
    static int printk(const char *fmt, ...) { (void)fmt; return 0; }
    '''
    suffix = r'''
    int main(void)
    {
     struct mt76_phy mphy={0}; struct mt792x_dev dev={0};
     struct mt792x_phy phy={&dev,&mphy}; struct ieee80211_hw hw={&phy};
     struct ieee80211_vif vif={.valid_links=7,.mld=true};
     struct mt792x_vif *mvif=&vif.drv_priv;
     struct channel chans[4]={{6},{36},{37},{44}};
     struct ieee80211_chanctx_conf ctx[4]={0};
     struct mt792x_bss_conf links[3]={0};
     int failures=0;
     mvif->phy=&phy; mvif->valid_links=6;
     for(unsigned i=0;i<3;i++) {
      ctx[i].def.chan=&chans[i]; ctx[i].drv_priv[0].bss_conf=&links[i];
      links[i].vif=mvif; links[i].link_id=i; links[i].mt76.idx=i;
      links[i].mt76.ctx=&ctx[i]; mvif->link_conf[i]=&links[i];
      bss_confs[i].link_id=i;
     }
     mvif->bss_conf=links[2];
     ctx[3].def.chan=&chans[3]; ctx[3].drv_priv[0].bss_conf=&links[2];
     /* Expected targets derive from literal link0/ch6 and link2/ch37 fixtures. */
     const unsigned which[]={2,0,3};
     const unsigned wants[]={2,0,99};
     const char *names[]={"assigned survivor", "resident incoming omitted by driver mask", "unassigned ctx cannot clobber survivor"};
     for(unsigned i=0;i<3;i++) {
      calls=puncture_calls=0; target=99; channel=0;
      mt7927_mlo_assoc_change_chanctx(&hw,&ctx[which[i]],IEEE80211_CHANCTX_CHANGE_PUNCTURING);
      unsigned want_calls=i==2 ? 0 : 1;
      if(calls!=want_calls || target!=wants[i] || locks || puncture_calls!=want_calls ||
         (i==0 && channel!=37) || (i==1 && channel!=6)) {
       printf("FAIL %s: calls=%u link=%u channel=%u puncture=%u\n",names[i],calls,target,channel,puncture_calls); failures++;
      } else printf("PASS %s\n",names[i]);
     }
     if(invalidations!=2) { puts("FAIL pair readiness must expire on owned MLO channel changes"); failures++; }
     /* Single-link operation and an unowned context retain their old contracts. */
     vif.mld=false; mvif->bss_conf=links[2]; calls=0; target=99;
     mt7927_mlo_assoc_change_chanctx(&hw,&ctx[2],0);
     if(calls!=1 || target!=2 || channel!=37 || locks) { puts("FAIL non-MLO primary"); failures++; }
     else puts("PASS non-MLO primary");
     ctx[3].drv_priv[0].bss_conf=NULL; calls=0;
     mt7927_mlo_assoc_change_chanctx(&hw,&ctx[3],0);
     if(calls || locks) { puts("FAIL unowned context"); failures++; } else puts("PASS unowned context");
     return failures ? 1 : 0;
    }
    '''
    with tempfile.TemporaryDirectory(prefix='mt7925-chanctx-') as tmp:
     cfile=Path(tmp)/'test.c'; binary=Path(tmp)/'test'
     cfile.write_text(prefix+function+suffix)
     subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',str(cfile),'-o',str(binary)],check=True)
     result = subprocess.run([str(binary)]).returncode
     if result:
      raise AssertionError('channel-routing fixture failed: %s' % result)

    # Break caught: assigning the 5-GHz chanctx of active03 must not replace its
    # already pair-assigned slot1 with slot0 merely because dormant L2 is valid.
    # Compile the actual resolver, assignment helper and chanctx callback. The
    # firmware calls below are recording boundaries, not simulated admission.
    def extract_function(signature):
     start = source.index(signature)
     end = source.index('\n}', start) + 2
     return source[start:end]

    assignment = '\n'.join(extract_function(signature) for signature in (
     'static u8 mt7927_mlo_5g6g_band_idx(',
     'static void mt7927_mld_assign_band_idx(',
     'int mt7927_mlo_assoc_assign_vif_chanctx('))
    assignment_prefix = r'''
    #include <stdbool.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <stdarg.h>
    typedef uint8_t u8;
    typedef uint16_t u16;
    enum nl80211_band { NL80211_BAND_2GHZ=0, NL80211_BAND_5GHZ=1, NL80211_BAND_6GHZ=3 };
    #define IEEE80211_MLD_MAX_NUM_LINKS 3
    #define NL80211_IFTYPE_STATION 2
    #define NL80211_IFTYPE_MONITOR 6
    #define BIT(i) (1UL << (i))
    #define READ_ONCE(x) (x)
    #define KERN_INFO ""
    #define for_each_set_bit(i,p,n) for((i)=0;(i)<(n);(i)++) if(*(p)&BIT(i))
    static unsigned hweight16(u16 v) { return __builtin_popcount(v); }
    struct ieee80211_vif;
    struct ieee80211_channel { unsigned hw_value; enum nl80211_band band; };
    struct ieee80211_bss_conf { unsigned link_id; struct ieee80211_vif *vif; struct { struct { struct ieee80211_channel *chan; } oper; } chanreq; };
    struct mt792x_bss_conf;
    struct mt792x_chanctx { struct mt792x_bss_conf *bss_conf; };
    struct ieee80211_chanctx_conf { struct { struct ieee80211_channel *chan; } def; struct mt792x_chanctx drv_priv[1]; };
    struct mt792x_bss_conf { struct { struct ieee80211_chanctx_conf *ctx; unsigned idx; u8 band_idx; } mt76; unsigned link_id; };
    struct mt792x_dev { struct { int mutex; bool mt7927; void *dev; } mt76; int phy; };
    struct mt792x_phy { void *mt76; };
    struct mt792x_vif { struct mt792x_bss_conf bss_conf, *link_conf[3]; struct { struct { struct { u8 phy_idx; } wcid; } deflink; } sta; unsigned deflink_id; struct mt792x_phy *phy; };
    struct ieee80211_vif { u16 valid_links, active_links; int type; bool mld; struct mt792x_vif drv_priv[1]; };
    struct ieee80211_hw { struct mt792x_dev *dev; };
    static struct ieee80211_bss_conf *confs[3];
    static bool mt76_mlo_diag_trace, mt7925_mlo_ctl_trace, mt7925_mlo_chanctx_keep_band;
    static struct mt792x_dev *mt792x_hw_dev(struct ieee80211_hw *h) { return h->dev; }
    static bool is_mt7927(void *m) { return ((struct mt792x_dev *)m)->mt76.mt7927; }
    static void mt76_phy_chandef_set(void *p, void *d) { (void)p; (void)d; }
    static bool ieee80211_vif_is_mld(struct ieee80211_vif *v) { return v->mld; }
    static struct ieee80211_bss_conf *mt792x_vif_to_bss_conf(struct ieee80211_vif *v,unsigned id) { (void)v; return confs[id]; }
    static struct mt792x_bss_conf *mt792x_vif_to_link(struct mt792x_vif *v,unsigned id) { return v->link_conf[id] ? v->link_conf[id] : &v->bss_conf; }
    static u8 mt7927_band_idx(enum nl80211_band b) { return b != NL80211_BAND_2GHZ; }
    static void mutex_lock(int *m) { ++*m; }
    static void mutex_unlock(int *m) { --*m; }
    static void dev_info(void *d,const char *fmt,...) { (void)d; (void)fmt; }
    static int printk(const char *fmt,...) { (void)fmt; return 0; }
    static int mt7927_reconfig_band(struct mt792x_dev *d,struct ieee80211_vif *v,struct ieee80211_bss_conf *b,struct mt792x_bss_conf *m,u8 slot) { (void)d;(void)v;(void)b;m->mt76.band_idx=slot;return 0; }
    static int mt7925_mcu_add_bss_info(void *p,void *c,void *b,void *s,bool on) { (void)p;(void)c;(void)b;(void)s;(void)on;return 0; }
    static void mt7925_monitor_arm_sniffer(void *p,void *v,void *c) { (void)p;(void)v;(void)c; }
    '''
    assignment_cases = r'''
    int main(void) {
     struct mt792x_dev dev={.mt76.mt7927=true}; struct ieee80211_hw hw={&dev};
     struct ieee80211_vif vif={.valid_links=7,.type=NL80211_IFTYPE_STATION,.mld=true};
     struct mt792x_vif *mvif=vif.drv_priv;
     struct ieee80211_channel channels[3]={{11,NL80211_BAND_2GHZ},{64,NL80211_BAND_5GHZ},{117,NL80211_BAND_6GHZ}};
     struct mt792x_phy fixture_phy={0};
     struct ieee80211_bss_conf bss[3]={0}; struct mt792x_bss_conf member[3]={0};
     struct ieee80211_chanctx_conf ctx[3]={0};
     /* Expectations are literal hardware-context contracts, not resolver output. */
     const struct { const char *name; u16 active; unsigned id, slot, want; bool chip, mld; } cases[]={
      {"03 incoming5 retains slot1 with dormant6 registered",3,1,1,1,true,true},
      {"03 anchor24 remains slot0",3,0,0,0,true,true},
      {"05 incoming6 remains slot1",5,2,1,1,true,true},
      {"05 anchor24 remains slot0",5,0,0,0,true,true},
      {"06 configured5 context remains slot0",6,1,0,0,true,true},
      {"06 configured6 context remains slot1",6,2,1,1,true,true},
      {"initial singleton5 retains registered-set initialization",2,1,1,0,true,true},
      {"dormant5 retains provisional registered-set initialization",5,1,1,0,true,true},
      {"other-chip path unchanged",3,1,1,1,false,true},
      {"non-MLD path unchanged",0,1,0,1,true,false}
     };
     unsigned failures=0;
     for(unsigned i=0;i<3;i++) { bss[i].link_id=i;bss[i].vif=&vif;bss[i].chanreq.oper.chan=&channels[i];confs[i]=&bss[i];ctx[i].def.chan=&channels[i];member[i].link_id=i;mvif->link_conf[i]=&member[i]; }
     mvif->deflink_id=0; mvif->phy=&fixture_phy;
     for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
      unsigned id=cases[i].id; vif.active_links=cases[i].active;vif.mld=cases[i].mld;dev.mt76.mt7927=cases[i].chip;
      struct mt792x_bss_conf *m=id ? &member[id] : &mvif->bss_conf;
      mvif->link_conf[0]=&mvif->bss_conf;
      if(!vif.mld) m=&mvif->bss_conf;
      m->mt76.band_idx=cases[i].slot;
      int ret=mt7927_mlo_assoc_assign_vif_chanctx(&hw,&vif,&bss[id],&ctx[id]);
      if(ret || m->mt76.band_idx!=cases[i].want || m->mt76.ctx!=&ctx[id] || ctx[id].drv_priv[0].bss_conf!=m || dev.mt76.mutex) {
       printf("FAIL %s: slot=%u want=%u ret=%d\n",cases[i].name,m->mt76.band_idx,cases[i].want,ret); failures++;
      } else printf("PASS %s\n",cases[i].name);
     }
     return failures ? 1 : 0;
    }
    '''
    with tempfile.TemporaryDirectory(prefix='mt7925-chanctx-assign-') as tmp:
     cfile=Path(tmp)/'test.c'; binary=Path(tmp)/'test'
     cfile.write_text(assignment_prefix+assignment+assignment_cases)
     subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',str(cfile),'-o',str(binary)],check=True)
     subprocess.run([str(binary)], check=True)


class ProductionFixture(unittest.TestCase):
    def test_channel_assignment_and_routing(self):
        run_fixture()


if __name__ == "__main__":
    if len(sys.argv) == 2 and Path(sys.argv[1]).is_dir():
        run_fixture(Path(sys.argv[1]))
    else:
        unittest.main()
