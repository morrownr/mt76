#!/usr/bin/env python3
"""Execute the real radio event owner against host request/grant fixtures."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(path, name):
    text = (ROOT / path).read_text()
    match = re.search(r"(?:static\s+)?(?:bool|void|int|u8)\s+" + name +
                      r"\([^;{]*\)\s*\{", text)
    if not match:
        raise AssertionError(f"definition missing: {path}:{name}")
    start = match.start()
    depth, end = 1, match.end()
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


EVENT_PREFIX = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#include "mt7925/mlo_roc_state.h"
#define READ_ONCE(x) (x)
#define KERN_INFO ""
#define HZ 100
#define MT76_STATE_ROC 0
#define MT7925_ROC_REQ_ROC 1
#define MT7925_ROC_REQ_JOIN 0
#define UNI_EVENT_ROC_GRANT 0
#define IEEE80211_IFACE_ITER_RESUME_ALL 0
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define WARN_ON_ONCE(x) ((void)(x))
#define printk(...) ((void)0)
#define pr_info(...) ((void)0)
#define dev_info(...) ((void)0)
#define wake_up(p) ((void)(p))
#define jiffies_to_msecs(x) (x)
#define msecs_to_jiffies(x) (x)
#define clear_bit(b,p) (*(p) &= ~(1UL << (b)))
#define ieee80211_ready_on_channel(...) ((void)0)
#define mod_timer(t,expires) ((void)(expires))
static unsigned long jiffies;
static int mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
struct tlv { u16 tag, len; };
struct mt7925_roc_grant_tlv {
 u16 tag, len; u8 bss_idx, tokenid, status, primarychannel, rfsco,
 rfband, channelwidth, centerfreqseg1, centerfreqseg2, reqtype, dbdcband;
 u32 max_interval;
};
struct ieee80211_hw { int unused; };
struct mt76_phy { unsigned long state; };
struct mt792x_phy {
 struct mt7925_mlo_roc_state mlo_roc;
 struct mt76_phy *mt76;
 bool roc_grant, roc_attach_done, sublink_roc_done;
 u8 sublink_roc_token_id, sublink_roc_status;
 int roc_wait, roc_timer;
};
struct mt76_dev { struct ieee80211_hw *hw; void *dev; bool dut; };
struct mt792x_dev { struct mt76_dev mt76; struct mt792x_phy phy; };
#define NL80211_IFTYPE_STATION 1
#define IEEE80211_MLD_MAX_NUM_LINKS 3
#define for_each_set_bit(i,p,n) for ((i)=0;(i)<(n);(i)++) if (*(p)&(1UL<<(i)))
struct ieee80211_channel { unsigned int hw_value; };
struct ieee80211_bss_conf {
 struct { struct { struct ieee80211_channel *chan; } oper; } chanreq;
};
struct mt792x_bss_conf { struct { u8 idx; } mt76; };
struct mt792x_vif { struct mt792x_phy *phy; struct mt792x_bss_conf links[3]; };
struct ieee80211_vif {
 int type; bool mld; struct mt792x_vif drv_priv[1];
 struct ieee80211_bss_conf conf[3];
};
static struct ieee80211_vif fixture_vif;
#define ieee80211_vif_is_mld(v) ((v)->mld)
#define mt792x_vif_to_link(v,id) (&(v)->links[id])
#define mt792x_vif_to_bss_conf(v,id) (&(v)->conf[id])
#define ieee80211_iterate_active_interfaces_atomic(hw,flags,fn,p) \
 fn(p,NULL,&fixture_vif)
struct mt7927_mlo_radio_match {
 struct mt792x_phy *phy; struct mt7925_roc_grant_tlv *grant; bool matched;
};
#define is_mt7927(d) ((d)->dut)
static void mt7925_trace_radio_grant(struct mt792x_dev *d,
 const char *tag, struct mt7925_roc_grant_tlv *g) { }
"""

class RadioExecutionTest(unittest.TestCase):
    def test_delta_acquire_serializes_firmware_selected_band(self):
        # Break caught: transmitting the expected physical slot instead of
        # ACQUIRE's firmware-selected sentinel, or ignoring MLSR policy.
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef u16 __le16; typedef u32 __le32;
#define __packed __attribute__((packed))
#include "mt7925/mlo_reqtype_test.h"
#define READ_ONCE(x) (x)
#define KERN_INFO ""
#define printk(...) ((void)0)
#define pr_info(...) ((void)0)
#define print_hex_dump(...) ((void)0)
#define UNI_ROC_ACQUIRE 0
#define UNI_ROC_SUB_LINK 3
#define IEEE80211_MLD_CAP_OP_MAX_SIMUL_LINKS 15
#define IEEE80211_MLD_MAX_NUM_LINKS 3
#define NL80211_BAND_2GHZ 0
#define NL80211_BAND_5GHZ 1
#define NL80211_BAND_6GHZ 2
#define CMD_CBW_20MHZ 0
#define EPERM 1
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define MCU_UNI_CMD(x) 7
#define for_each_set_bit(i,p,n) for ((i)=0;(i)<(n);(i)++) if (*(p)&(1UL<<(i)))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
enum mt7925_roc_req { MT7925_ROC_REQ_JOIN=0, MT7925_ROC_REQ_MLSR_AG=10,
 MT7925_ROC_REQ_MLSR_AA=11 };
static int mt7925_mlo_roc_dbdc_policy, mt7925_mlo_test_reqtype=-1;
static bool mt7925_mlo_test_reqtype_initial;
static int mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
struct ieee80211_channel { unsigned band, center_freq, hw_value; };
struct ieee80211_bss_conf { struct { struct { struct ieee80211_channel *chan; } oper; } chanreq; };
struct mt792x_vif { int unused; };
struct ieee80211_vif { struct mt792x_vif drv_priv; struct ieee80211_bss_conf conf[3]; };
#define mt792x_vif_to_bss_conf(v,id) (&(v)->conf[id])
struct mt792x_bss_conf { struct mt792x_vif *vif; unsigned link_id; struct { u8 idx,band_idx; } mt76; };
struct wiphy { struct { unsigned mld_capa_and_ops; } iftype_ext_capab[1]; };
struct hw { struct wiphy *wiphy; };
struct mt76_phy { struct hw *hw; };
struct mt792x_dev { struct { bool dut; } mt76; };
struct mt7925_mlo_grant_inc { int unused; };
struct mt792x_phy { struct mt792x_dev *dev; struct mt76_phy *mt76;
 struct { struct mt7925_mlo_grant_inc grant[3]; } mlo_roc; };
#define is_mt7927(d) ((d)->dut)
#define mt7925_mlo_continuity_get(s) (-1)
#define ieee80211_frequency_to_channel(f) 36
struct mt7925_radio_req_rec { bool valid; u8 token,bss_idx,link_id,dbdcband; };
static struct mt7925_radio_req_rec mt7925_radio_req_recs[4];
static unsigned mt7925_radio_req_rec_n;
static unsigned char wire[256]; static unsigned wire_len, sends;
static int mt76_mcu_send_msg(void *d,int cmd,void *p,unsigned n,bool wait) {
 memcpy(wire,p,n); wire_len=n; sends++; return 0;
}
"""
        header = (ROOT / "mt7925/mcu.h").read_text()
        tlv = re.search(r"struct roc_acquire_tlv \{.*?\} __packed;", header, re.S).group()
        body = "\n".join(function("mt7925/mlo/radio.c", name) for name in (
            "mt7927_mlo_join_roc_dbdcband", "mt7927_mlo_radio_reqtype",
            "mt7927_mlo_radio_test_reqtype", "mt7927_mlo_radio_slot_dbdc"))
        body += "\n" + function("mt7925/mcu.c", "mt7925_mcu_set_mlo_single_acquire")
        suffix = r"""
int main(void) {
 struct mt792x_dev dev={.mt76={.dut=true}};
 struct wiphy wiphy={.iftype_ext_capab={{1}}}; struct hw hw={&wiphy};
 struct mt76_phy mphy={&hw}; struct mt792x_phy phy={.dev=&dev,.mt76=&mphy};
 struct ieee80211_channel chan={1,5180,36}; struct ieee80211_vif vif={0};
 struct mt792x_bss_conf bss={.vif=&vif.drv_priv,.link_id=1,.mt76={7,1}};
 vif.conf[1].chanreq.oper.chan=&chan;
 const struct { int slot,policy,override; unsigned caps,band,type,dbdc; } cases[]={
  {0,0,-1,1,1,0,254}, {1,0,-1,1,1,0,254},
  {1,0,-1,1,0,0,254}, {1,0,-1,0,1,11,255},
  {1,0,-1,0,0,10,255}, {1,0,10,1,1,10,255},
  {1,0,11,1,1,11,255}, {1,1,-1,1,1,0,0},
  {1,2,-1,1,1,0,1}, {1,3,-1,1,1,0,255},
  {1,4,-1,1,1,0,254}
 };
 for(unsigned i=0;i<ARRAY_SIZE(cases);i++) {
  mt7925_mlo_roc_dbdc_policy=cases[i].policy;
  mt7925_mlo_test_reqtype=cases[i].override;
  wiphy.iftype_ext_capab[0].mld_capa_and_ops=cases[i].caps;
  chan.band=cases[i].band; sends=0;
  if(mt7925_mcu_set_mlo_single_acquire(&phy,&bss,2,5,42,cases[i].slot)) return 1;
  struct roc_acquire_tlv *r=(void *)(wire+4);
  if(sends!=1 || wire_len!=4+sizeof(*r) || r->tag!=0 || r->tokenid!=42 ||
     r->bss_idx!=7 || r->reqtype!=cases[i].type || r->dbdcband!=cases[i].dbdc) {
   fprintf(stderr,"wire case=%u type=%u dbdc=%u len=%u sends=%u\n",
    i,r->reqtype,r->dbdcband,wire_len,sends); return 1;
  }
 }
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "acquire.c", Path(tmp) / "acquire"
            src.write_text(prefix + tlv + body + suffix)
            built = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror",
                                    "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                                    "-I", str(ROOT), str(src), "-o", str(exe)],
                                   text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_beacon_loss_does_not_disconnect_a_multi_active_mld(self):
        # Break caught: treating a link's firmware BSS event as a global
        # disconnect while multiple links are active, or accepting foreign BSS.
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
#define IEEE80211_MLD_MAX_NUM_LINKS 3
#define READ_ONCE(x) (x)
#define KERN_INFO ""
#define printk(...) ((void)0)
#define BIT(n) (1U<<(n))
#define hweight16(x) __builtin_popcount(x)
#define for_each_set_bit(i,p,n) for ((i)=0;(i)<(n);(i)++) if (*(p)&(1UL<<(i)))
static int mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
struct mt792x_bss_conf { struct { u8 idx; } mt76; };
struct mt792x_vif { struct mt792x_bss_conf *link[3]; };
struct ieee80211_vif { void *drv_priv; unsigned valid_links, active_links; };
#define mt792x_vif_to_link(v,id) ((v)->link[id])
static unsigned disconnects;
static void ieee80211_connection_loss(struct ieee80211_vif *v) { (void)v; disconnects++; }
"""
        body = function("mt7925/mlo/radio.c", "mt7927_mlo_radio_beacon_loss")
        suffix = r"""
int main(void) {
 struct mt792x_bss_conf bss[3]={{{10}},{{11}},{{12}}};
 struct mt792x_vif mvif={.link={&bss[0],&bss[1],&bss[2]}};
 struct ieee80211_vif vif={.drv_priv=&mvif,.valid_links=7};
 const struct { unsigned active, bss, expected; } cases[]={
  {6,11,0},{5,12,0},{3,10,0},{4,12,1},{2,11,1},{1,10,1},{4,99,0},
  /* a valid but inactive link's firmware event never disconnects the healthy sole active link */
  {4,11,0},{4,10,0},{2,10,0},{2,12,0},{1,11,0},{1,12,0}
 };
 int failed=0;
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
  vif.active_links=cases[i].active; disconnects=0;
  mt7927_mlo_radio_beacon_loss(&vif,cases[i].bss);
  if(disconnects!=cases[i].expected) {
   fprintf(stderr,"beacon case=%u disconnects=%u\n",i,disconnects);failed++;
  }
 }
 return failed ? 1 : 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "beacon.c", Path(tmp) / "beacon"
            src.write_text(prefix + body + suffix)
            built = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror",
                                    "-Wno-unused-variable", str(src), "-o", str(exe)],
                                   text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_slot_policy_preserves_mt7927_and_generic_wire_choices(self):
        # Break caught: applying an installed slot to ACQUIRE/A-G, applying
        # MT7927 policy to generic hardware, or leaking delta reqtype override.
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
#include "mt7925/mlo_reqtype_test.h"
#define READ_ONCE(x) (x)
#define KERN_INFO ""
#define printk(...) ((void)0)
#define UNI_ROC_ACQUIRE 0
#define UNI_ROC_SUB_LINK 3
#define MT7925_ROC_REQ_JOIN 0
#define MT7925_ROC_REQ_MLSR_AG 10
#define MT7925_ROC_REQ_MLSR_AA 11
#define IEEE80211_MLD_CAP_OP_MAX_SIMUL_LINKS 0xF
static int mt7925_mlo_roc_dbdc_policy, mt7925_mlo_test_reqtype=-1;
static bool mt7925_mlo_test_reqtype_initial;
static int mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
struct mt792x_bss_conf { unsigned link_id; struct { u8 idx, band_idx; } mt76; };
struct ieee80211_channel { unsigned center_freq; };
struct wiphy { struct { unsigned mld_capa_and_ops; } iftype_ext_capab[1]; };
struct hw { struct wiphy *wiphy; };
struct mt76_phy { struct hw *hw; };
struct mt792x_dev { struct { bool dut; } mt76; };
struct mt792x_phy { struct mt792x_dev *dev; struct mt76_phy *mt76; };
#define is_mt7927(d) ((d)->dut)
"""
        body = "\n".join(function("mt7925/mlo/radio.c", name) for name in (
            "mt7927_mlo_join_roc_dbdcband", "mt7927_mlo_radio_reqtype",
            "mt7927_mlo_radio_slot_dbdc"))
        suffix = r"""
int main(void) {
 struct mt792x_dev dev={.mt76={.dut=true}};
 struct wiphy wiphy={.iftype_ext_capab={{1}}}; struct hw hw={&wiphy};
 struct mt76_phy mphy={&hw}; struct mt792x_phy phy={&dev,&mphy};
 struct mt792x_bss_conf bss={.link_id=1,.mt76={.idx=1,.band_idx=0}};
 struct ieee80211_channel chan={5180}; int failed=0;
 const struct { unsigned policy; bool ag; u16 tag; u8 ordinal, expected; } cases[]={
  {0,false,0,0,254}, {0,false,3,1,0}, {0,true,3,1,254},
  {1,false,0,0,0}, {1,false,3,1,1}, {2,false,0,1,0},
  {3,false,3,1,255}, {4,false,3,1,254}, {4,true,3,1,254}
 };
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
  mt7925_mlo_roc_dbdc_policy=cases[i].policy;
  unsigned out=mt7927_mlo_radio_slot_dbdc(&phy,&bss,&chan,0,cases[i].ag,
   cases[i].tag,cases[i].ordinal,false);
  if(out!=cases[i].expected) { fprintf(stderr,"slot case=%u got=%u\n",i,out);failed++; }
 }
 dev.mt76.dut=false; mt7925_mlo_roc_dbdc_policy=3;
 if(mt7927_mlo_radio_slot_dbdc(&phy,&bss,&chan,0,false,3,1,false)!=1 ||
    mt7927_mlo_radio_slot_dbdc(&phy,&bss,&chan,0,true,3,1,false)!=254 ||
    mt7927_mlo_radio_slot_dbdc(&phy,&bss,&chan,10,false,3,1,false)!=255) failed++;
 mt7925_mlo_test_reqtype=10;
 if(mt7927_mlo_radio_reqtype(&phy,false,false)!=0 ||
    mt7927_mlo_radio_reqtype(&phy,false,true)!=10) failed++;
 mt7925_mlo_test_reqtype_initial=1; /* only with the explicit TEST flag does the override reach the association JOIN */
 if(mt7927_mlo_radio_reqtype(&phy,false,false)!=10) failed++;
 mt7925_mlo_test_reqtype_initial=0;
 mt7925_mlo_test_reqtype=-1; wiphy.iftype_ext_capab[0].mld_capa_and_ops=0;
 if(mt7927_mlo_radio_reqtype(&phy,false,false)!=11 ||
    mt7927_mlo_radio_reqtype(&phy,true,false)!=10) failed++;
 return failed ? 1 : 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "policy.c", Path(tmp) / "policy"
            src.write_text(prefix + body + suffix)
            built = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror",
                                    "-Wno-unused-variable", "-I", str(ROOT),
                                    str(src), "-o", str(exe)], text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_malformed_firmware_tlvs_never_reach_grant_owner(self):
        # Break caught: zero/short/out-of-frame TLV lengths either loop
        # forever or dispatch an incomplete grant to the state owner.
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
struct tlv { u16 tag, len; };
struct mt7925_mcu_rxd { char bytes[4]; };
struct mt7925_roc_grant_tlv { u16 tag, len; char fields[20]; };
struct mt792x_dev { struct { void *dev; } mt76; };
struct sk_buff { unsigned char *data; unsigned int len; };
#define le16_to_cpu(x) (x)
#define READ_ONCE(x) (x)
#define KERN_INFO ""
#define DUMP_PREFIX_OFFSET 0
#define UNI_EVENT_ROC_GRANT 0
#define UNI_EVENT_ROC_GRANT_SUB_LINK 4
/* verbatim from mt7925/mcu.h: the validated TLV iteration the ROC event loop uses */
static inline bool
mt7925_mcu_tlv_valid(struct tlv *tlv, u32 rem)
{
	u16 len;

	if (rem < sizeof(*tlv))
		return false;

	len = le16_to_cpu(tlv->len);

	/* a length below the header size would not advance the cursor */
	return len >= sizeof(*tlv) && len <= rem;
}

#define mt7925_for_each_tlv(tlv, rem)					\
	for (; mt7925_mcu_tlv_valid(tlv, rem);				\
	     (rem) -= le16_to_cpu((tlv)->len),				\
	     (tlv) = (struct tlv *)((u8 *)(tlv) + le16_to_cpu((tlv)->len)))
#define dev_info(...) ((void)0)
#define print_hex_dump(...) ((void)0)
static int mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
static int dispatched;
static bool pskb_may_pull(struct sk_buff *s, unsigned int n) { return n<=s->len; }
static void skb_pull(struct sk_buff *s, unsigned int n) { s->data+=n; s->len-=n; }
static void mt7925_mcu_roc_handle_grant(struct mt792x_dev *d, struct tlv *t) {
 dispatched++;
}
static void mt7925_mcu_roc_handle_grant_sub_link(struct mt792x_dev *d, struct tlv *t) {
 dispatched++;
}
"""
        suffix = r"""
int main(void) {
 struct mt792x_dev dev={0}; unsigned char frame[64]={0}; int failures=0;
 struct tlv *tlv=(void *)(frame+8);
 const unsigned int frame_len[]={32,12,12,32,10,7};
 const unsigned int tlv_len[]={24,0,3,60,24,24};
 for (int i=0;i<6;i++) {
  struct sk_buff skb={.data=frame,.len=frame_len[i]};
  tlv->tag=0; tlv->len=tlv_len[i]; dispatched=0;
  mt7925_mcu_uni_roc_event(&dev,&skb);
  if (dispatched != (i==0)) {
   fprintf(stderr,"frame=%d dispatched=%d\n",i,dispatched);failures++;
  }
 }
 return failures ? 1 : 0;
}
"""
        source = prefix + function("mt7925/mcu.c", "mt7925_mcu_uni_roc_event") + suffix
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "decode.c", Path(tmp) / "decode"
            src.write_text(source)
            built = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror",
                                    "-Wno-unused-variable", "-Wno-unused-function",
                                    str(src), "-o", str(exe)], text=True,
                                   capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            try:
                result = subprocess.run([str(exe)], text=True,
                                        capture_output=True, timeout=1)
            except subprocess.TimeoutExpired:
                self.fail("malformed ROC stream failed to make progress")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stale_or_failed_join_cannot_complete_current_request(self):
        # Break caught: accepting any GRANT while pending_links is nonzero
        # falsely turns an old/failed request into the current JOIN's grant.
        prefix = EVENT_PREFIX
        radio = ROOT / "mt7925/mlo/radio.c"
        if radio.exists():
            body = "\n".join(function("mt7925/mlo/radio.c", name) for name in (
                "mt7927_mlo_radio_reject",
                "mt7925_mlo_radio_single_event", "mt7925_mlo_radio_attach_event",
                "mt7927_mlo_radio_match_iter",
                "mt7927_mlo_radio_event"))
            dispatch = "mt7927_mlo_radio_event(&dev, &event, false);"
        else:
            body = "\n".join(function("mt7925/mcu.c", name) for name in (
                "mt7925_mcu_roc_single_event", "mt7925_mcu_roc_attach_event",
                "mt7925_mcu_roc_handle_grant"))
            dispatch = "mt7925_mcu_roc_handle_grant(&dev, (struct tlv *)&event);"
        suffix = r"""
int main(void) {
 static struct ieee80211_hw hw = {0}; static struct mt76_phy mphy = {0};
 static struct mt792x_dev dev = {.mt76 = {.hw=&hw, .dut=true}, .phy={.mt76=&mphy}};
 struct mt7925_roc_grant_tlv event = {.tag=UNI_EVENT_ROC_GRANT,
 .len=sizeof(event), .tokenid=8, .bss_idx=1, .primarychannel=36,
 .reqtype=MT7925_ROC_REQ_JOIN};
 static struct ieee80211_channel channels[3] = {{1},{36},{5}};
 fixture_vif.type=NL80211_IFTYPE_STATION; fixture_vif.mld=true;
 fixture_vif.drv_priv[0].phy=&dev.phy;
 for (int id=0;id<3;id++) {
  fixture_vif.drv_priv[0].links[id].mt76.idx=id;
  fixture_vif.conf[id].chanreq.oper.chan=&channels[id];
 }
 int failures=0;
 for (int which=0; which<5; which++) {
  memset(&dev.phy.mlo_roc, 0, sizeof(dev.phy.mlo_roc));
  dev.phy.mlo_roc.held_token=7; dev.phy.mlo_roc.held_links=4;
  dev.phy.mlo_roc.pending_token=9; dev.phy.mlo_roc.pending_links=3;
  dev.phy.roc_grant=false;
  event.tokenid=which==0 ? 8 : 9; event.status=which==1 ? 1 : 0;
  event.bss_idx=which==3 ? 2 : 1;
  event.primarychannel=which==4 ? 40 : 36;
  DISPATCH
  if (dev.phy.roc_grant != (which==2) ||
      dev.phy.mlo_roc.held_token != 7 || dev.phy.mlo_roc.held_links != 4) {
   fprintf(stderr,"case=%d completion=%d held=%u/%u\n",which,
    dev.phy.roc_grant,dev.phy.mlo_roc.held_token,dev.phy.mlo_roc.held_links);
   failures++;
  }
 }
 memset(&dev.phy.mlo_roc, 0, sizeof(dev.phy.mlo_roc));
 dev.phy.mlo_roc.held_token=7; dev.phy.mlo_roc.held_links=4;
 dev.phy.mlo_roc.attach_pending_token=7;
 dev.phy.mlo_roc.attach_pending_links=2;
 dev.phy.mlo_roc.attach_pending_bss=2; /* BSS 1 mask, not index 2 */
 event.tokenid=7; event.bss_idx=1; event.status=1;
 dev.phy.roc_attach_done=false;
 DISPATCH
 if (dev.phy.roc_attach_done || dev.phy.mlo_roc.held_links != 4) {
  fprintf(stderr,"failed attach completed=%d held=%u\n",
   dev.phy.roc_attach_done,dev.phy.mlo_roc.held_links);failures++;
 }
 memset(&dev.phy.mlo_roc, 0, sizeof(dev.phy.mlo_roc));
 dev.phy.mlo_roc.timedout_token=9; event.tokenid=9; event.status=1;
 DISPATCH
 if (dev.phy.mlo_roc.orphan_bss) {
  fprintf(stderr,"rejected late event invented orphan=%u\n",
   dev.phy.mlo_roc.orphan_bss);failures++;
 }
 return failures ? 1 : 0;
}
""".replace("DISPATCH", dispatch)
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "radio.c", Path(tmp) / "radio"
            src.write_text(prefix + body + suffix)
            built = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror",
                                    "-Wno-unused-variable", "-Wno-unused-function",
                                    "-I", str(ROOT), str(src), "-o", str(exe)],
                                   text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
