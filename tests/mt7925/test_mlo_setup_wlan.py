#!/usr/bin/env python3
"""Execute the actual MLD builder: distinguish primary from setup WCID.

The default and non-MT7927 payloads must remain unchanged. The diagnostic gate
may change only the setup WCID, while both serialized link slots retain the
current active pair. This does not prove firmware admission or secure RX.
"""
from pathlib import Path
import subprocess
import tempfile

import unittest

def run_fixture(root=None):
    root = Path(root) if root is not None else Path(__file__).resolve().parents[2]
    source = (root / 'mt7925/mlo/peer.c').read_text()
    signature = 'int\n'
    start = source.index(signature+'mt7925_mcu_sta_mld_tlv(')
    end = source.index('\n}\n', start) + 3
    builder = source[start:end]
    prefix = r'''
    #include <stdbool.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <string.h>
    #include <errno.h>
    typedef uint8_t u8;
    typedef uint16_t u16;
    typedef uint16_t __le16;
    #define ETH_ALEN 6
    #define IEEE80211_MLD_MAX_NUM_LINKS 3
    #define IEEE80211_LINK_UNSPECIFIED 255
    #define BIT(n) (1UL << (n))
    #define READ_ONCE(x) (x)
    #define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
    #define hweight_long(x) __builtin_popcountl(x)
    static bool fixture_mld = true;
    #define ieee80211_vif_is_mld(v) (fixture_mld)
    #define cpu_to_le16(x) (x)
    #define le16_to_cpu(x) (x)
    #define KERN_INFO ""
    #define printk(...) ((void)0)
    #define dev_warn_ratelimited(...) ((void)0)
    #define for_each_set_bit(i, p, n) for ((i)=0; (i)<(n); (i)++) if (*(p)&BIT(i))
    #define STA_REC_MLD 0x20
    static bool mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
    static bool mt7925_mlo_test_setup_wlan_current;
    struct tlv { __le16 tag, len; };
    struct sta_rec_mld {
     __le16 tag, len;
     u8 mac_addr[ETH_ALEN];
     __le16 primary_id, secondary_id, wlan_id;
     u8 link_num, rsv[3];
     struct { __le16 wlan_id; u8 bss_idx, rsv; } link[2];
    };
    struct sk_buff { struct sta_rec_mld record; bool emitted; };
    struct mt792x_dev { struct { void *dev; bool chip7927; } mt76; };
    struct mt792x_phy { struct mt792x_dev *dev; };
    struct mt792x_bss_conf { unsigned link_id; struct { u8 idx; } mt76; };
    struct mt792x_link_sta { struct { unsigned idx, link_id; bool link_valid; } wcid; };
    struct mt792x_vif {
     struct mt792x_phy *phy;
     struct { unsigned long rebind_target_links; } mt7927_mlo;
     struct mt792x_bss_conf *link[3];
    };
    struct mt792x_sta {
     unsigned deflink_id;
     struct mt792x_link_sta deflink;
     struct mt792x_link_sta *link[3];
    };
    struct ieee80211_vif { void *drv_priv; unsigned long active_links; };
    struct ieee80211_sta { void *drv_priv; u8 addr[ETH_ALEN]; };
    static struct tlv *mt76_connac_mcu_add_tlv(struct sk_buff *s, unsigned tag, unsigned len)
    { s->emitted=true; s->record.tag=tag; s->record.len=len; return (struct tlv *)&s->record; }
    static struct mt792x_link_sta *mt792x_sta_to_link(struct mt792x_sta *s, unsigned id)
    { return s->link[id]; }
    static struct mt792x_bss_conf *mt792x_vif_to_link(struct mt792x_vif *v, unsigned id)
    { return v->link[id]; }
    static unsigned long mt7925_mlo_pt_mld_links(unsigned long active, unsigned long target)
    { return target ? target : active; }
    static bool is_mt7927(const void *p)
    { const struct mt792x_dev *d=p; return d->mt76.chip7927; }
    '''
    suffix = r'''
    int main(void)
    {
     struct mt792x_dev dev={.mt76={.chip7927=true}};
     struct mt792x_phy phy={.dev=&dev};
     struct mt792x_vif mvif={.phy=&phy};
     struct mt792x_sta msta={.deflink_id=2};
     struct ieee80211_vif vif={.drv_priv=&mvif};
     struct ieee80211_sta sta={.drv_priv=&msta, .addr={0x8c,0xed,0xe1,0x98,0xcc,0x18}};
     struct mt792x_bss_conf bss[3];
     struct mt792x_link_sta peer[3];
     const unsigned ids[3]={3,2,1}, pairs[][2]={{6,5},{5,3},{3,6},{6,3},{3,5},{5,6}};
     unsigned failures=0, checks=0;
     for (unsigned id=0;id<3;id++) {
      bss[id].link_id=id; bss[id].mt76.idx=id;
      peer[id].wcid.idx=ids[id]; peer[id].wcid.link_id=id; peer[id].wcid.link_valid=true;
      mvif.link[id]=&bss[id]; msta.link[id]=&peer[id];
     }
     for (unsigned p=0;p<6;p++) for (unsigned mode=0;mode<3;mode++) {
      unsigned old=pairs[p][0], next=pairs[p][1], incoming=__builtin_ctz(next & ~old);
      unsigned primary=(next & BIT(2)) ? 2 : __builtin_ctz(next);
      unsigned secondary=__builtin_ctz(next & ~BIT(primary));
      struct sk_buff skb={0};
      vif.active_links=old & next; /* mac80211 has published the shrink, not the grow. */
      mt7925_mlo_test_setup_wlan_current=mode!=0;
      dev.mt76.chip7927=mode!=2;
      mt7925_mcu_sta_mld_tlv(&skb,&vif,&sta,&bss[incoming],&peer[incoming],NULL);
      unsigned setup=mode==1 ? ids[incoming] : ids[primary]; checks++;
      if (!skb.emitted || skb.record.primary_id!=ids[primary] ||
          skb.record.secondary_id!=ids[secondary] || skb.record.wlan_id!=setup ||
          skb.record.link_num!=2 || skb.record.link[0].wlan_id!=ids[primary] ||
          skb.record.link[1].wlan_id!=ids[secondary] ||
          skb.record.link[0].bss_idx!=primary || skb.record.link[1].bss_idx!=secondary ||
          memcmp(skb.record.mac_addr,sta.addr,ETH_ALEN)) {
       printf("FAIL 0x%x->0x%x mode=%u primary=%u setup=%u wanted_setup=%u links=%u\n",
        old,next,mode,skb.record.primary_id,skb.record.wlan_id,setup,skb.record.link_num);
       failures++;
      }
     }
     /* A survivor refresh must keep the complete target pair with the same primary. */
     dev.mt76.chip7927=true; mt7925_mlo_test_setup_wlan_current=true;
     vif.active_links=4; mvif.mt7927_mlo.rebind_target_links=5;
     struct sk_buff skb={0};
     mt7925_mcu_sta_mld_tlv(&skb,&vif,&sta,&bss[2],&peer[2],NULL); checks++;
     if (!skb.emitted || skb.record.primary_id!=1 || skb.record.wlan_id!=1 ||
         skb.record.link_num!=2 || skb.record.secondary_id!=3) {
      puts("FAIL anchor refresh changed pair membership"); failures++;
     }
     /* A declared topology cannot silently omit a missing or wrong-link
      * object and still be submitted as success. link_valid is separately
      * a traffic-eligibility gate, not proof of firmware admission.
      * This is validation only, not a new ownership encoding.
      */
     mvif.mt7927_mlo.rebind_target_links=0; vif.active_links=4;
     for (unsigned bad=0;bad<5;bad++) {
      struct sk_buff invalid={0};
      mvif.link[0]=&bss[0]; msta.link[0]=&peer[0];
      peer[0].wcid.link_valid=true; peer[0].wcid.link_id=0;
      if(bad==0) mvif.link[0]=NULL;
      if(bad==1) mvif.link[0]=&bss[2]; /* real lookup may fall back to default BSS */
      if(bad==2) msta.link[0]=NULL;
      if(bad==3) peer[0].wcid.link_valid=false;
      if(bad==4) peer[0].wcid.link_id=1;
      int ret=BUILD(&invalid,&vif,&sta,&bss[0],&peer[0],NULL); checks++;
      if(bad==3) {
       /* Real migration authorizes while the subject's TX gate is parked.
        * Retain existing filtered representation; do not block authorization
        * or mutate eligibility in the serializer.
        */
       if(ret || !invalid.emitted || invalid.record.link_num!=1 ||
          peer[0].wcid.link_valid) {
        printf("FAIL parked authorization ret=%d emitted=%u\n",ret,invalid.emitted);
        failures++;
       }
      } else if(ret!=-EINVAL || invalid.emitted) {
       printf("FAIL invalid membership %u ret=%d emitted=%u links=%u\n",
        bad,ret,invalid.emitted,invalid.record.link_num); failures++;
      }
     }
     mvif.link[0]=&bss[0]; msta.link[0]=&peer[0];
     peer[0].wcid.link_valid=true; peer[0].wcid.link_id=0;
     vif.active_links=7; struct sk_buff overfull={0};
     int ret=BUILD(&overfull,&vif,&sta,&bss[0],&peer[0],NULL); checks++;
     if(ret!=-E2BIG || overfull.emitted) {
      printf("FAIL fixed two-slot truncation ret=%d emitted=%u links=%u\n",
       ret,overfull.emitted,overfull.record.link_num); failures++;
     }
     /* Vendor mldStarecUpdateMldId explicitly assigns secondary=primary
      * for a one-member group. Zero instead names an unrelated WCID.
      */
     vif.active_links=4; struct sk_buff single={0};
     ret=BUILD(&single,&vif,&sta,&bss[2],&peer[2],NULL); checks++;
     if(ret || !single.emitted || single.record.link_num!=1 ||
        single.record.primary_id!=1 || single.record.secondary_id!=1 ||
        single.record.wlan_id!=1 || single.record.link[0].wlan_id!=1) {
      printf("FAIL single member secondary ret=%d primary=%u secondary=%u\n",
       ret,single.record.primary_id,single.record.secondary_id); failures++;
     }
     msta.deflink_id=IEEE80211_LINK_UNSPECIFIED;
     struct sk_buff no_primary={0};
     ret=BUILD(&no_primary,&vif,&sta,&bss[2],&peer[2],NULL); checks++;
     if(ret!=-EINVAL || no_primary.emitted) { puts("FAIL missing primary accepted"); failures++; }
     fixture_mld=false; struct sk_buff legacy={0};
     ret=BUILD(&legacy,&vif,&sta,NULL,NULL,NULL); checks++;
     if(ret || legacy.emitted) { puts("FAIL non-MLD behavior changed"); failures++; }
     printf("%u checks, %u failures\n",checks,failures);
     return failures ? 1 : 0;
    }
    '''
    with tempfile.TemporaryDirectory(prefix='.setup-wlan-test-', dir=root) as temp:
        c = Path(temp) / 'test.c'
        binary = Path(temp) / 'test'
        invocation = ('#define BUILD(...) mt7925_mcu_sta_mld_tlv(__VA_ARGS__)\n'
                      if signature=='int\n' else
                      '#define BUILD(...) ({mt7925_mcu_sta_mld_tlv(__VA_ARGS__);0;})\n')
        c.write_text(prefix + builder + invocation + suffix)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-Wno-unused-function', str(c), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


class ProductionFixture(unittest.TestCase):
    def test_actual_mld_builder(self):
        run_fixture()


if __name__ == "__main__":
    unittest.main()
