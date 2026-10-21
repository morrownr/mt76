#!/usr/bin/env python3
"""Run the real pair-band selector and activation loop with MCU recorders.

The regression is a resident incoming link that retains the requested band
number while that radio's DEV address belongs to a different affiliated link.
These checks prove command routing and error handling, not firmware RX.
"""
from pathlib import Path
import subprocess
import tempfile

import unittest

def run_fixture(root=None):
    root = Path(root) if root is not None else Path(__file__).resolve().parents[2]
    source = (root / 'mt7925/mlo/lifecycle.c').read_text()
    start = source.index('mt7927_mlo_pair_band_idx(')
    end = source.index('\nstatic int\nmt7927_mlo_reassign_band', start)
    selector = 'static u8\n' + source[start:end]
    start = source.index('mt7927_mlo_reassign_active_bands(')
    end = source.index('\nstatic int\nmt7927_mlo_rebuild_peer_assoc_order', start)
    activation = 'static int\n' + source[start:end]
    # The pre-fix interface has no activation mask. This adapter lets the same
    # behavioral fixtures demonstrate its missing DEV writes before the API edit.
    has_mask = 'activate_links' in activation.split('{', 1)[0]
    prefix = r'''
    #include <stdbool.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <errno.h>
    typedef uint8_t u8;
    typedef uint16_t u16;
    #define BIT(n) (1UL << (n))
    #define IEEE80211_MLD_MAX_NUM_LINKS 3
    #define READ_ONCE(x) (x)
    #define KERN_INFO ""
    #define printk(...) ((void)0)
    static bool mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
    static bool mt7925_test_mt7927_pair56_reverse;
    static bool mt7925_test_mt7927_pair56_same_slot;
    enum nl80211_band { NL80211_BAND_2GHZ, NL80211_BAND_5GHZ, NL80211_BAND_6GHZ };
    struct ieee80211_channel { enum nl80211_band band; };
    struct ieee80211_bss_conf { struct { struct { struct ieee80211_channel *chan; } oper; } chanreq; };
    struct ieee80211_vif { int unused; };
    struct mt792x_dev { int mphy; };
    struct mt792x_sta { int unused; };
    struct mt792x_vif { struct mt792x_sta sta; };
    struct mt792x_bss_conf { unsigned link_id; struct { u8 band_idx; unsigned idx; } mt76; };
    struct mt792x_link_sta { struct { unsigned idx; } wcid; };
    static struct ieee80211_channel channels[3] = {
     { NL80211_BAND_2GHZ }, { NL80211_BAND_5GHZ }, { NL80211_BAND_6GHZ }
    };
    static struct ieee80211_bss_conf confs[3];
    static struct mt792x_bss_conf bss[3];
    static struct mt792x_link_sta bmc[3];
    static int owner[2], writes[3], dev_error_link, missing_link;
    static unsigned first_error_writes;
    static unsigned hweight16(u16 mask) { return __builtin_popcount(mask); }
    static unsigned __ffs(unsigned long mask) { return __builtin_ctzl(mask); }
    #define for_each_set_bit(i, p, n) for ((i)=0; (i)<(n); (i)++) if (*(p)&BIT(i))
    static struct ieee80211_bss_conf *mt792x_vif_to_bss_conf(void *v, unsigned id)
    { (void)v; return id == (unsigned)missing_link ? NULL : &confs[id]; }
    static struct mt792x_bss_conf *mt7927_mlo_resident_bss(void *d, void *v, unsigned id)
    { (void)d; (void)v; return &bss[id]; }
    static struct mt792x_link_sta *mt7927_mlo_resident_sta(void *d, void *s, unsigned id)
    { (void)d; (void)s; return &bmc[id]; }
    static int mt76_connac_mcu_uni_add_dev(void *phy, struct ieee80211_bss_conf *conf,
     void *mconf, void *wcid, bool enable)
    {
     (void)phy; (void)mconf; (void)wcid;
     unsigned id = conf - confs;
     if (!enable) return -EINVAL;
     writes[id]++;
     if (id == (unsigned)dev_error_link) { first_error_writes++; return -EIO; }
     owner[bss[id].mt76.band_idx] = id;
     return 0;
    }
    /* Existing band migration already emits DEV enable. Replace only that
     * dependency with a recorder; unchanged bands deliberately emit nothing.
     */
    static int mt7927_mlo_reassign_band(struct mt792x_dev *d, struct ieee80211_vif *v,
     struct mt792x_vif *m, unsigned id, u8 band)
    {
     (void)v; (void)m;
     if (band == 255) return -EINVAL;
     if (bss[id].mt76.band_idx == band) return 0;
     bss[id].mt76.band_idx = band;
     return mt76_connac_mcu_uni_add_dev(&d->mphy, &confs[id], &bss[id].mt76, &bmc[id].wcid, true);
    }
    '''
    adapter = r'''
    static int run(struct mt792x_dev *d, struct ieee80211_vif *v,
     struct mt792x_vif *m, u16 pair, u16 added)
    {
     ADAPTER_CALL
    }
    '''.replace('ADAPTER_CALL',
               'return mt7927_mlo_reassign_active_bands(d,v,m,pair,added);' if has_mask else
               '(void)added; return mt7927_mlo_reassign_active_bands(d,v,m,pair);')
    suffix = r'''
    static void reset(unsigned old)
    {
     owner[0]=owner[1]=-1;
     missing_link=dev_error_link=-1; first_error_writes=0;
     for (unsigned id=0; id<3; id++) {
      confs[id].chanreq.oper.chan=&channels[id];
      bss[id].link_id=id; bss[id].mt76.idx=id;
      /* Dormant contexts retain the previous allocation. */
      bss[id].mt76.band_idx=id==2 ? 1 : 0;
      writes[id]=0;
     }
     for (unsigned id=0; id<3; id++) if (old & BIT(id)) {
      unsigned band=mt7927_mlo_pair_band_idx(NULL,old,id);
      bss[id].mt76.band_idx=band; owner[band]=id;
     }
    }
    int main(void)
    {
     const unsigned pairs[][2]={{6,5},{5,3},{3,6},{6,3},{3,5},{5,6}};
     struct mt792x_dev d={0}; struct ieee80211_vif v={0}; struct mt792x_vif m={0};
     unsigned failures=0, checks=0;
     for(unsigned i=0;i<6;i++) {
      unsigned old=pairs[i][0], next=pairs[i][1], added=next&~old;
      reset(old); unsigned incoming=__ffs(added);
      int ret=run(&d,&v,&m,next,added); checks++;
      if(ret || writes[incoming]!=1 || owner[bss[incoming].mt76.band_idx]!=(int)incoming) {
       printf("FAIL 0x%x->0x%x incoming=%u DEV writes=%d owner=%d ret=%d\n",
        old,next,incoming,writes[incoming],owner[bss[incoming].mt76.band_idx],ret); failures++;
      }
     }
     reset(6); int ret=run(&d,&v,&m,6,0); checks++;
     if(ret || writes[1] || writes[2]) { puts("FAIL unchanged pair rewrites survivor DEV"); failures++; }
     reset(6); dev_error_link=0; ret=run(&d,&v,&m,5,1); checks++;
     if(ret!=-EIO || first_error_writes!=1 || writes[2]) { puts("FAIL incoming DEV rejection must propagate"); failures++; }
     reset(6); ret=run(&d,&v,&m,7,1); checks++;
     if(ret!=-EINVAL || writes[0] || writes[1] || writes[2]) { puts("FAIL three-active pair must reject before writes"); failures++; }
     printf("%u checks, %u failures\n",checks,failures);
     return failures ? 1 : 0;
    }
    '''
    with tempfile.TemporaryDirectory(prefix='.slot-activation-test-', dir=root) as temp:
        c = Path(temp) / 'test.c'
        binary = Path(temp) / 'test'
        c.write_text(prefix + selector + activation + adapter + suffix)
        # The trace globals are consumed only by the post-fix diagnostic print.
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-Wno-unused-function', str(c), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


class ProductionFixture(unittest.TestCase):
    def test_slot_activation_and_error_paths(self):
        run_fixture()


if __name__ == "__main__":
    unittest.main()
