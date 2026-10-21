#!/usr/bin/env python3
"""Execute the production MLD serializer with unpublished peer fixtures.

Kernel allocation and lookups are replaced at the boundary; membership and
serialization execute the function extracted unchanged from mlo/peer.c.
Removing pending-pointer membership must fail before any peer publication.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class PendingMldTest(unittest.TestCase):
    def test_primary_record_contains_unpublished_peer_for_all_directed_legs(self):
        source = (ROOT / "mt7925/mlo/peer.c").read_text()
        start = source.index("int\nmt7925_mcu_sta_mld_tlv(")
        end = source.index("\n}\n", start) + 3
        builder = source[start:end]
        header = (ROOT / "mt7925/mcu.h").read_text()
        record = header[header.index("struct sta_rec_mld {"):]
        record = record[:record.index("\n} __packed;") + len("\n} __packed;")]
        has_pending = "struct mt792x_link_sta *pending" in builder.split("\n{")[0]
        call = "mt7925_mcu_sta_mld_tlv(skb, vif, sta, bss, subject"
        call += ", pending);" if has_pending else "); (void)pending;"
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t __le16;
#define __packed __attribute__((packed))
#define ETH_ALEN 6
#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define hweight_long(x) __builtin_popcountl(x)
#define ieee80211_vif_is_mld(v) (true)
#define READ_ONCE(x) (x)
#define cpu_to_le16(x) (x)
#define le16_to_cpu(x) (x)
#define IEEE80211_LINK_UNSPECIFIED 255
#define IEEE80211_MLD_MAX_NUM_LINKS 16
#define for_each_set_bit(i, m, n) for ((i)=0; (i)<(n); (i)++) if (*(m) & BIT(i))
#define KERN_INFO ""
#define printk(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define STA_REC_MLD 0x20
struct mt792x_link_sta { struct { unsigned idx, link_id; bool link_valid; } wcid; };
struct mt792x_bss_conf { struct { unsigned idx; } mt76; unsigned link_id; };
struct mt792x_sta { unsigned deflink_id; struct mt792x_link_sta deflink;
    struct mt792x_link_sta *link[16]; };
struct mt792x_dev { struct { void *dev; } mt76; };
struct phy { struct mt792x_dev *dev; };
struct mt792x_vif { struct phy *phy; struct { unsigned rebind_target_links; } mt7927_mlo;
    struct mt792x_bss_conf *link[16]; };
struct ieee80211_vif { void *drv_priv; unsigned active_links; };
struct ieee80211_sta { void *drv_priv; u8 addr[6]; };
struct tlv { __le16 tag, len; };
struct sk_buff { unsigned char data[64]; unsigned len; };
static int mt7925_mlo_test_setup_wlan_current, mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
static struct mt792x_link_sta *mt792x_sta_to_link(struct mt792x_sta *s, unsigned i)
{ return s->link[i]; }
static struct mt792x_bss_conf *mt792x_vif_to_link(struct mt792x_vif *s, unsigned i)
{ return s->link[i]; }
static unsigned mt7925_mlo_pt_mld_links(unsigned a, unsigned b) { return a | b; }
static int is_mt7927(void *p) { (void)p; return 1; }
static struct tlv *mt76_connac_mcu_add_tlv(struct sk_buff *s, unsigned tag, unsigned len)
{ struct tlv *p=(void *)s->data; memset(s->data, 0, sizeof(s->data));
  s->len=len; p->tag=tag; p->len=len; return p; }
'''
        fixtures = r'''
static void build(struct sk_buff *skb, struct ieee80211_vif *vif,
    struct ieee80211_sta *sta, struct mt792x_bss_conf *bss,
    struct mt792x_link_sta *subject, struct mt792x_link_sta *pending)
{ memset(skb,0,sizeof(*skb)); CALL }
static int check(unsigned old, unsigned target)
{
    unsigned survivor=__builtin_ctz(old & target), incoming=__builtin_ctz(target & ~old);
    /* Existing active-primary policy retains deflink 2 when active, else
     * picks the lowest active link. Membership must match after publication.
     */
    unsigned primary=(target & 4) ? 2 : 0;
    unsigned secondary=__builtin_ctz(target & ~BIT(primary));
    struct mt792x_dev dev={0}; struct phy phy={.dev=&dev};
    struct mt792x_vif mvif={.phy=&phy}; struct mt792x_sta msta={.deflink_id=2};
    struct ieee80211_vif vif={.drv_priv=&mvif, .active_links=BIT(survivor)};
    struct ieee80211_sta sta={.drv_priv=&msta, .addr={2,3,4,5,6,7}};
    struct mt792x_link_sta peers[3]={0}; struct mt792x_bss_conf bss[3]={0};
    struct sk_buff skb={0}; struct sta_rec_mld *mld=(void *)skb.data;
    for (unsigned i=0; i<3; i++) {
        peers[i].wcid.idx=i+1; peers[i].wcid.link_id=i;
        peers[i].wcid.link_valid=(i==survivor || i==incoming);
        bss[i].link_id=i; bss[i].mt76.idx=10+i; mvif.link[i]=&bss[i];
        msta.link[i]=(i==incoming ? NULL : &peers[i]);
    }
    /* Command subject is stable host primary, which can have been retired. */
    build(&skb, &vif, &sta, &bss[2], &peers[2], &peers[incoming]);
    if (mld->link_num!=2 || mld->primary_id!=primary+1 ||
        mld->secondary_id!=secondary+1 || mld->link[0].bss_idx!=10+primary ||
        mld->link[1].bss_idx!=10+secondary || memcmp(mld->mac_addr,sta.addr,6)) {
        fprintf(stderr,"old=0x%x target=0x%x: expected survivor+pending; got n=%u primary=%u secondary=%u\n",
            old,target,mld->link_num,mld->primary_id,mld->secondary_id); return 1;
    }
    if (msta.link[incoming]) return 2; /* serializer must not publish it */
    struct sta_rec_mld before=*mld;
    msta.link[incoming]=&peers[incoming]; vif.active_links=target;
    build(&skb,&vif,&sta,&bss[survivor],&peers[survivor],NULL);
    if (memcmp(&before,mld,sizeof(before))) return 3;
    /* Missing BSS or invalid peer must not become an encoded pending slot. */
    msta.link[incoming]=NULL; vif.active_links=BIT(survivor);
    mvif.link[incoming]=NULL;
    build(&skb,&vif,&sta,&bss[survivor],&peers[survivor],&peers[incoming]);
    if (skb.len && mld->link_num!=1) return 4;
    mvif.link[incoming]=&bss[incoming]; peers[incoming].wcid.link_valid=false;
    build(&skb,&vif,&sta,&bss[survivor],&peers[survivor],&peers[incoming]);
    return skb.len && mld->link_num!=1 ? 5 : 0;
}
static int initial_assoc(void) {
    struct mt792x_dev dev={0}; struct phy phy={.dev=&dev};
    struct mt792x_vif mvif={.phy=&phy}; struct mt792x_sta msta={.deflink_id=2};
    struct ieee80211_vif vif={.drv_priv=&mvif,.active_links=0};
    struct ieee80211_sta sta={.drv_priv=&msta};
    struct mt792x_link_sta primary={.wcid={.idx=1,.link_id=2,.link_valid=true}};
    struct mt792x_link_sta pending={.wcid={.idx=2,.link_id=1,.link_valid=true}};
    struct mt792x_bss_conf p_bss={.mt76={.idx=0},.link_id=2};
    struct mt792x_bss_conf s_bss={.mt76={.idx=1},.link_id=1};
    struct sk_buff skb={0}; struct sta_rec_mld *mld=(void *)skb.data;
    msta.link[2]=&primary; mvif.link[2]=&p_bss; mvif.link[1]=&s_bss;
    build(&skb,&vif,&sta,&p_bss,&primary,&pending);
    if (!skb.len || mld->link_num!=2 || mld->primary_id!=1 || mld->secondary_id!=2) {
        fprintf(stderr,"initial active=0: expected primary1/secondary2; got n=%u primary=%u secondary=%u\n",
            mld->link_num,mld->primary_id,mld->secondary_id); return 6;
    }
    return 0;
}
int main(void) {
    const unsigned pairs[]={6,5,3};
    for(unsigned i=0;i<3;i++) for(unsigned j=0;j<3;j++) if(i!=j) {
        int r=check(pairs[i],pairs[j]); if(r) return r;
    }
    int r=initial_assoc(); if(r) return r;
    puts("pending MLD serializer: all six directed legs and initial association passed"); return 0;
}
'''.replace("CALL", call)
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "pending.c"
            exe = Path(tmp) / "pending"
            c.write_text(prelude + record + builder + fixtures)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
