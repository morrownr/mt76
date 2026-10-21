#!/usr/bin/env python3
"""RX conversion must retain the WCID's link identity across the 7.3 API change."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RxLinkApiTest(unittest.TestCase):
    def test_rx_accounting_and_rearm_follow_real_evidence_and_generation(self):
        """Catch a missing reject gate or timestamp/rearm on a stale epoch."""
        text = (ROOT / "mt7925/mlo/datapath.c").read_text()

        def function(marker):
            start = text.index(marker)
            opening = text.index("{", start)
            depth, end = 1, opening + 1
            while depth:
                depth += (text[end] == "{") - (text[end] == "}")
                end += 1
            return text[start:end]

        production = "\n".join([
            function("static int\nmt7925_mlo_rx_wrong_channel("),
            function("static void\nmt7925_mlo_account_valid_rx("),
            function("void mt7925_mlo_rx_account("),
        ])
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "mt7925/mlo_valid_rx.h"
typedef uint8_t u8;
typedef uint16_t __le16;
typedef uint32_t u32;
#define IEEE80211_MLD_MAX_NUM_LINKS 16
#define BIT(n) (1UL << (n))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define rcu_dereference(p) (p)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define KERN_INFO ""
#define printk(...) ((void)0)
#define pr_info_ratelimited(...) ((void)0)
#define RX_FLAG_8023 1u
#define RX_FLAG_DECRYPTED 2u
#define MT_RXD1_NORMAL_CM 1u
#define MT_RXD1_NORMAL_CLM 2u
#define MT_RXD1_NORMAL_ICV_ERR 4u
#define MT_RXD1_NORMAL_TKIP_MIC_ERR 8u
#define MT_RXD3_NORMAL_FCS_ERR 1u
#define MT_RXD2_NORMAL_SEC_MODE 3u
#define FIELD_GET(mask,value) ((value)&(mask))
struct channel { int center_freq; };
struct ieee80211_bss_conf { struct { struct { struct channel *chan; } oper; } chanreq; };
struct mt792x_vif { struct { unsigned int transition_generation; } mt7927_mlo; };
struct ieee80211_vif { unsigned long active_links; struct ieee80211_bss_conf *link_conf[16]; struct mt792x_vif drv_priv; };
struct mt792x_sta { struct mt792x_vif *vif; unsigned long mlo_link_ready_jiffies[16]; };
struct mt76_wcid { unsigned int idx,link_id; bool link_valid; u8 hw_key_idx; };
struct mt792x_link_sta { struct mt76_wcid wcid; struct mt792x_sta *sta; unsigned int mlo_valid_rx_packets,mlo_await_generation; unsigned long mlo_valid_rx_jiffies; bool mlo_await_valid_rx; };
struct mt76_rx_status { struct mt76_wcid *wcid; unsigned int flag; int freq; };
struct sk_buff { _Alignas(16) unsigned char cb[256]; };
static bool mt7925_mlo_rx_chan_check,mt76_mlo_diag_trace,mt7925_mlo_ctl_trace;
static unsigned long jiffies=1000;
static bool ieee80211_is_data(__le16 fc) { return (fc&12)==8; }
static bool ieee80211_is_nullfunc(__le16 fc) { return fc==0x48; }
static bool ieee80211_is_qos_nullfunc(__le16 fc) { return fc==0xc8; }
"""
        cases = r"""
int main(void) {
 struct testcase {
  const char *name; unsigned int rxd1,rxd2,rxd3,flags,fc;
  bool unicast,no_wcid,inactive,invalid,stale,ready,channel_check,wrong_channel,unkeyed;
  unsigned int packets; unsigned long want_ready; bool want_wait;
 } cases[]={
  {"protected data rearms current generation",0,1,0,2,8, 1,0,0,0,0,0,0,0,0, 1,1000,0},
  {"ICV failure rejects despite decrypted flag",4,1,0,2,8, 1,0,0,0,0,0,0,0,0, 0,0,1},
  {"cipher mismatch rejects",1,1,0,2,8, 1,0,0,0,0,0,0,0,0, 0,0,1},
  {"cipher length mismatch rejects",2,1,0,2,8, 1,0,0,0,0,0,0,0,0, 0,0,1},
  {"FCS failure rejects",0,1,1,2,8, 1,0,0,0,0,0,0,0,0, 0,0,1},
  {"TKIP MIC failure rejects",8,1,0,2,8, 1,0,0,0,0,0,0,0,0, 0,0,1},
  {"protected undecrypted rejects",0,1,0,0,8, 1,0,0,0,0,0,0,0,0, 0,0,1},
  {"stale generation counts RX but cannot rearm",0,1,0,2,8, 1,0,0,0,1,0,0,0,0, 1,0,1},
  {"inactive member cannot rearm",0,1,0,2,8, 1,0,1,0,0,0,0,0,0, 1,0,1},
  {"invalid WCID cannot rearm",0,1,0,2,8, 1,0,0,1,0,0,0,0,0, 1,0,1},
  {"existing readiness stays stable",0,1,0,2,8, 1,0,0,0,0,1,0,0,0, 1,777,1},
  {"wrong channel cannot become proof",0,1,0,2,8, 1,0,0,0,0,0,1,1,0, 0,0,1},
  {"default-off channel check preserves behavior",0,1,0,2,8, 1,0,0,0,0,0,0,1,0, 1,1000,0},
  {"unresolved WCID has no accounting",0,1,0,2,8, 1,1,0,0,0,0,0,0,0, 0,0,1},
  {"multicast has no readiness proof",0,1,0,2,8, 0,0,0,0,0,0,0,0,0, 0,0,1},
  {"null data has no readiness proof",0,1,0,2,0x48, 1,0,0,0,0,0,0,0,0, 0,0,1},
  {"plain data on keyed peer rejects",0,0,0,0,8, 1,0,0,0,0,0,0,0,0, 0,0,1},
  {"plain unkeyed data preserves existing acceptance",0,0,0,0,8, 1,0,0,0,0,0,0,0,1, 1,1000,0},
  {"translated Ethernet data remains evidence",0,1,0,3,0, 1,0,0,0,0,0,0,0,0, 1,1000,0},
 };
 int failures=0;
 for (unsigned int n=0;n<sizeof(cases)/sizeof(cases[0]);n++) {
  struct testcase *t=&cases[n]; struct sk_buff skb={0};
  struct channel ch={.center_freq=5180};
  struct ieee80211_bss_conf conf={.chanreq.oper.chan=&ch};
  struct ieee80211_vif vif={.active_links=t->inactive?0:2,.link_conf={[1]=&conf},.drv_priv.mt7927_mlo.transition_generation=7};
  struct mt792x_sta sta={.vif=&vif.drv_priv,.mlo_link_ready_jiffies={[1]=t->ready?777:0}};
  struct mt792x_link_sta link={.wcid={.idx=3,.link_id=1,.link_valid=!t->invalid,.hw_key_idx=t->unkeyed?255:0},.sta=&sta,.mlo_await_valid_rx=true,.mlo_await_generation=t->stale?6:7};
  struct mt76_rx_status *status=(void *)skb.cb;
  status->wcid=t->no_wcid?NULL:&link.wcid;status->flag=t->flags;status->freq=t->wrong_channel?6135:5180;
  mt7925_mlo_rx_chan_check=t->channel_check;
  mt7925_mlo_rx_account(&skb,t->unicast,t->fc,t->rxd1,t->rxd2,t->rxd3);
  bool ok=link.mlo_valid_rx_packets==t->packets && link.mlo_valid_rx_jiffies==(t->packets?1000:0) && sta.mlo_link_ready_jiffies[1]==t->want_ready && link.mlo_await_valid_rx==t->want_wait;
  printf("%s %s packets=%u ready=%lu waiting=%u\n",ok?"PASS":"FAIL",t->name,link.mlo_valid_rx_packets,sta.mlo_link_ready_jiffies[1],link.mlo_await_valid_rx);
  failures+=!ok;
 }
 return failures?1:0;
}
"""
        with tempfile.TemporaryDirectory(prefix="mlo-rx-account-") as tmp:
            source = Path(tmp) / "account.c"
            source.write_text(prefix + production + cases)
            executable = Path(tmp) / "account"
            build = subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(source), "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_rx_resolver_checks_bss_band_before_accepting_raw_wcid(self):
        """Catch accepting WCID phy0 for L2/BSS band1 on descriptor band0.

        Compile the actual production resolver block and decision header. Only
        kernel object/RCU boundaries are doubled; expected link IDs are literal
        topology outcomes, not computed by the resolver under test.
        """
        owner = ROOT / "mt7925/mlo/datapath.c"
        self.assertTrue(owner.exists(), "RX execution owner has not been implemented")
        text = owner.read_text()

        def braced(start):
            opening = text.index("{", start)
            depth = 1
            end = opening + 1
            while depth:
                depth += (text[end] == "{") - (text[end] == "}")
                end += 1
            return text[start:end]

        retired = braced(text.index("static bool mt7925_mlo_rx_link_retired("))
        block = "\n".join([
            braced(text.index("static void\nmt7925_mlo_rx_route_band(")),
            braced(text.index("static void\nmt7925_mlo_rx_route_freq(")),
            braced(text.index("void mt7925_mlo_rx_route(")),
        ])
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mt7925/mlo_rx_band_resolve.h"
#include "mt7925/mlo_rx_freq_resolve.h"
typedef uint8_t u8;
typedef uint16_t u16;
#define IEEE80211_MLD_MAX_NUM_LINKS 16
#define BIT(x) (1UL << (x))
#define READ_ONCE(x) (x)
#define rcu_dereference(x) (x)
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define unlikely(x) (x)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define MT_RXD1_NORMAL_BAND_IDX 3u
#define FIELD_GET(mask, value) ((value) & (mask))
#define KERN_INFO ""
struct mt792x_sta;
struct mt76_wcid { unsigned int idx, phy_idx, link_id; bool link_valid; };
struct mt792x_link_sta { struct mt76_wcid wcid; struct mt792x_sta *sta; };
struct mt792x_bss_conf { struct { unsigned int band_idx; } mt76; };
struct channel { unsigned int center_freq; };
struct ieee80211_bss_conf { struct { struct { struct channel *chan; } oper; } chanreq; };
struct mt792x_vif { struct mt792x_bss_conf *link_conf[16]; };
struct ieee80211_vif { unsigned long active_links; struct ieee80211_bss_conf *link_conf[16]; struct mt792x_vif drv_priv; };
struct mt792x_sta { unsigned long valid_links; struct mt792x_link_sta *link[16]; struct mt792x_vif *vif; };
struct mt76_dev { int chip; };
struct mt792x_dev { struct mt76_dev mt76; };
struct mt76_rx_status { struct mt76_wcid *wcid; unsigned int freq; };
static bool mt7925_mlo_rx_retired_aware;
static bool mt7925_mlo_rx_freq_attrib;
static bool mt76_mlo_diag_trace;
static bool is_mt7927(struct mt76_dev *dev) { return dev->chip == 0x7927; }
static int printk(const char *format, ...) { (void)format; return 0; }
#define printk_ratelimited printk
"""
        resolver = r"""
""" + block + r"""
static int resolve(struct mt792x_dev *dev, struct mt792x_link_sta *input,
                   unsigned int band, unsigned int freq, bool unicast) {
 struct mt76_rx_status saved = {.wcid = input ? &input->wcid : NULL, .freq = freq};
 mt7925_mlo_rx_route(dev, &saved, unicast, input ? input->wcid.idx : 0, band);
 return saved.wcid ? (int)saved.wcid->link_id : -1;
}
"""
        cases = r"""
int main(void) {
 struct testcase {
  const char *name; bool knob; int chip, raw, raw_phy, desc, want;
  unsigned int active; bool retired_l0, ambiguous, missing_bss;
  bool freq_knob; unsigned int freq; bool ambiguous_freq,multicast,no_vif;
 } cases[] = {
  {"stale L2 phy0, received band0 must resolve L0", 1,0x7927,2,0,0,0,5,0,0,0},
  {"L2 BSS band1, received band1 must retain L2", 1,0x7927,2,0,1,2,5,0,0,0},
  {"default-off retains legacy raw identity", 0,0x7927,2,0,0,2,5,0,0,0},
  {"non-MT7927 retains legacy raw identity", 1,0x7925,2,0,0,2,5,0,0,0},
  {"retired band0 candidate is excluded", 1,0x7927,2,0,0,2,4,1,0,0},
  {"ambiguous live band0 candidates do not guess", 1,0x7927,2,0,0,2,7,0,1,0},
  {"stale L0 phy1, received band1 must resolve L2", 1,0x7927,0,1,1,2,5,0,0,0},
  {"missing source BSS retains raw-band fallback", 1,0x7927,2,0,0,2,5,0,0,1},
  {"retired raw link resolves live incoming member", 1,0x7927,2,0,0,0,1,0,0,0},
  {"unresolved WCID is left unresolved", 1,0x7927,-1,0,0,-1,5,0,0,0},
  {"receive frequency remaps to incoming active L0",0,0x7927,2,0,0,0,5,0,0,0,1,2437,0,0},
  {"default-off frequency retains firmware choice",0,0x7927,2,0,0,2,5,0,0,0,0,2437,0,0},
  {"frequency cannot select inactive L0",0,0x7927,2,0,0,2,4,0,0,0,1,2437,0,0},
  {"ambiguous receive frequency does not guess",0,0x7927,2,0,0,2,7,0,0,0,1,2437,1,0},
  {"frequency override is MT7927-only",0,0x7925,2,0,0,2,5,0,0,0,1,2437,0,0},
  {"multicast retains raw link identity",0,0x7927,2,0,0,2,5,0,0,0,1,2437,0,1},
  {"missing vif uses safe legacy band candidates",1,0x7927,2,1,0,0,5,0,0,0,0,0,0,0,1},
 };
 int failures = 0;
 for (unsigned int n=0; n<sizeof(cases)/sizeof(cases[0]); n++) {
  struct testcase *t = &cases[n];
  struct mt792x_dev dev = {.mt76 = {.chip=t->chip}};
  struct ieee80211_vif vif = {.active_links=t->active};
  struct mt792x_sta peer = {.valid_links=t->ambiguous ? 7 : 5, .vif=&vif.drv_priv};
  struct mt792x_link_sta links[3] = {};
  struct mt792x_bss_conf bss[3] = {};
  struct channel chans[3] = {{2437},{5180},{6135}};
  struct ieee80211_bss_conf confs[3] = {};
  for (unsigned int lid=0; lid<3; lid++) {
   links[lid].wcid = (struct mt76_wcid){.idx=lid+1,.link_id=lid,
     .phy_idx=lid==2 ? 0 : lid,.link_valid=!!(t->active & BIT(lid))};
   links[lid].sta=&peer;
   peer.link[lid]=&links[lid]; vif.drv_priv.link_conf[lid]=&bss[lid];
   bss[lid].mt76.band_idx=lid ? 1 : 0;
   confs[lid].chanreq.oper.chan=&chans[lid];vif.link_conf[lid]=&confs[lid];
  }
  if (t->raw >= 0) links[t->raw].wcid.phy_idx=t->raw_phy;
  if (t->retired_l0) links[0].wcid.link_valid=false;
  if (t->ambiguous) bss[1].mt76.band_idx=0;
  if (t->missing_bss) vif.drv_priv.link_conf[t->raw]=NULL;
  mt7925_mlo_rx_retired_aware=t->knob;
  mt7925_mlo_rx_freq_attrib=t->freq_knob;
  if (t->ambiguous_freq) chans[1].center_freq=2437;
  if (t->no_vif) peer.vif=NULL;
  int got=resolve(&dev,t->raw >= 0 ? &links[t->raw] : NULL,t->desc,t->freq,!t->multicast);
  printf("%s: %s got link%d expected link%d\n",got==t->want?"PASS":"FAIL",t->name,got,t->want);
  failures+=got!=t->want;
 }
 printf("production RX resolver: %zu cases, %d failures\n",sizeof(cases)/sizeof(cases[0]),failures);
 return failures ? 1 : 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "resolver.c"
            source.write_text(prefix + retired + resolver + cases)
            exe = Path(tmp) / "resolver"
            build = subprocess.run(
                ["cc", "-Wall", "-Werror", "-I", str(ROOT), str(source), "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rx_uses_the_wcid_link_and_keeps_receive_metadata(self):
        text = (ROOT / "mac80211.c").read_text()
        start = text.index("static void\nmt76_rx_convert(")
        end = text.index("\nstatic void\n", start + 1)
        function = text[start:end]
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define KERNEL_VERSION(a,b,c) (((a)<<16)+((b)<<8)+(c))
#define LINUX_VERSION_CODE KERNEL_VERSION(7,API_MINOR,0)
#define rcu_dereference(p) (p)
#define BUILD_BUG_ON(x) _Static_assert(!(x), "size mismatch")
#define RX_ENC_EHT 5
#define RX_FLAG_NO_SIGNAL_VAL 1
#define IEEE80211_QOS_CTL_TID_MASK 15
struct ieee80211_link_sta { int id; };
struct ieee80211_sta { struct ieee80211_link_sta deflink, *link[16]; };
struct mt76_wcid { bool link_valid; int link_id; struct ieee80211_sta *sta; };
struct ieee80211_hw { int id; };
struct mt76_dev { struct ieee80211_hw hw; };
struct ieee80211_hdr { int frame_control; };
#define METADATA int flag,freq,enc_flags,encoding,bw,he_ru,he_gi,he_dcm,rate_idx,nss,band,signal,chains; struct { int ru,gi; } eht; int chain_signal[4];
struct ieee80211_rx_status { METADATA int ampdu_reference,device_timestamp,mactime,boottime_ns;
#if API_MINOR < 3
 bool link_valid; int link_id;
#endif
};
struct mt76_rx_status { METADATA int qos_ctl,ampdu_ref,timestamp,phy_idx; struct mt76_wcid *wcid; };
struct sk_buff { _Alignas(16) unsigned char cb[256]; int priority; struct ieee80211_hdr hdr; };
#define IEEE80211_SKB_RXCB(skb) ((struct ieee80211_rx_status *)(skb)->cb)
static struct ieee80211_hdr *mt76_skb_get_hdr(struct sk_buff *s) { return &s->hdr; }
static int mt76_rx_signal(int chains, int *signals) { return signals[0]; }
static int ieee80211_is_beacon(int control) { return control==1; }
static int ieee80211_is_probe_resp(int control) { return control==2; }
static int ktime_get_boottime_ns(void) { return 1234; }
static struct ieee80211_sta *wcid_to_sta(struct mt76_wcid *w) { return w?w->sta:NULL; }
static struct ieee80211_hw *mt76_phy_hw(struct mt76_dev *d, int idx) { return &d->hw; }
"""
        suffix = r"""
int main(void) {
 struct ieee80211_link_sta l1={1},l2={2};
 struct ieee80211_sta peer={.deflink={0},.link={[1]=&l1,[2]=&l2}};
 struct mt76_dev dev={0}; struct ieee80211_hw *hw=NULL;
 struct mt76_wcid wc={.link_valid=true,.link_id=1,.sta=&peer};
 int failures=0;
 for (int scenario=0;scenario<5;scenario++) {
  struct sk_buff skb={.hdr={1}};
  wc.link_valid=scenario!=2; wc.link_id=scenario==1?2:1;
  wc.sta=scenario==4?NULL:&peer;
  struct mt76_rx_status input={.wcid=scenario==3?NULL:&wc,.qos_ctl=7,
   .freq=5180,.encoding=RX_ENC_EHT,.eht={3,2},.chain_signal={-42},.timestamp=456};
  memcpy(skb.cb,&input,sizeof(input));
#if API_MINOR >= 3
  struct ieee80211_link_sta *link=NULL;
  mt76_rx_convert(&dev,&skb,&hw,&link);
  struct ieee80211_link_sta *expected=scenario==0?&l1:scenario==1?&l2:scenario==2?&peer.deflink:NULL;
  if (link!=expected) failures++;
#else
  struct ieee80211_sta *sta=NULL;
  mt76_rx_convert(&dev,&skb,&hw,&sta);
  if (sta!=(scenario>=3?NULL:&peer)) failures++;
  struct ieee80211_rx_status *legacy=IEEE80211_SKB_RXCB(&skb);
  if (legacy->link_valid!=(scenario!=2&&scenario!=3) ||
      legacy->link_id!=(scenario==3?0:scenario==1?2:1)) failures++;
#endif
  struct ieee80211_rx_status *out=IEEE80211_SKB_RXCB(&skb);
  if (hw!=&dev.hw || skb.priority!=7 || out->freq!=5180 || out->signal!=-42 ||
      out->eht.ru!=3 || out->eht.gi!=2 || out->mactime!=456 || out->boottime_ns!=1234) failures++;
 }
 printf("7.%d: 5 RX cases, %d failures\n",API_MINOR,failures);
 return failures?1:0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "rx.c"
            src.write_text(prefix + function + suffix)
            for minor in (2, 3):
                with self.subTest(kernel_minor=minor):
                    exe = Path(tmp) / f"rx{minor}"
                    build = subprocess.run(["cc", "-Wall", "-Werror", f"-DAPI_MINOR={minor}", str(src), "-o", str(exe)], capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
