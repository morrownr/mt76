#!/usr/bin/env python3
"""Execute the native-header block extracted verbatim from mt792x_tx().

Scope: host header mutation only, NOT firmware MAT, PN ownership, TXQ, or RF.
RCU and diagnostic output are stubbed; the production address writes execute.
The break caught is rewriting a station DATA addr3 (final DA) as a BSSID.
"""
import hashlib
import struct
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def native_block(source):
    start = source.index("\tif (vif && control->sta && ieee80211_vif_is_mld(vif) &&")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define ETH_ALEN 6
#define IEEE80211_TX_CTL_HW_80211_ENCAP 1
#define IEEE80211_TX_CTL_INJECTED 4
#define NL80211_IFTYPE_STATION 2
#define WLAN_CATEGORY_PROTECTED_EHT 37
#define KERN_INFO ""
#define READ_ONCE(x) (x)
#define unlikely(x) (x)
#define rcu_dereference(x) (x)
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define printk(...) ((void)0)
#define le16_to_cpu(x) (x)
static int mt76_mlo_diag_trace;
static bool mt7925_mlo_preserve_data_da = true;
static bool fixture_mt7927 = true, fixture_writable = true, fixture_relocate;
static int freed;
struct mt76_dev;
struct ieee80211_vif;
struct sk_buff;
struct mt76_driver_ops { bool (*tx_preserve_addr3)(struct mt76_dev *, struct ieee80211_vif *, struct sk_buff *); };
struct mt76_dev { bool mt7927; const struct mt76_driver_ops *drv; };
struct mt792x_dev { struct mt76_dev mt76; };
static bool is_mt7927(struct mt76_dev *dev) { return dev->mt7927; }
struct ieee80211_hdr { uint16_t frame_control, duration; uint8_t addr1[6], addr2[6], addr3[6]; uint16_t seq_ctrl; uint8_t addr4[6]; };
struct ieee80211_hdr_3addr { uint8_t data[24]; };
struct ieee80211_mgmt { uint16_t frame_control, duration; uint8_t da[6], sa[6], bssid[6]; uint16_t seq_ctrl; union { struct { uint8_t category, action_code; } action; } u; };
struct ieee80211_bss_conf { uint8_t addr[6], bssid[6]; };
struct ieee80211_link_sta { uint8_t addr[6]; };
struct ieee80211_sta { struct ieee80211_link_sta *link[16]; };
struct ieee80211_vif { int type; bool mld; struct ieee80211_bss_conf *link_conf[16]; };
struct ieee80211_tx_control { struct ieee80211_sta *sta; };
struct ieee80211_tx_info { unsigned flags; };
struct mt76_wcid { unsigned idx, link_id; };
struct sk_buff { unsigned char *data; unsigned len; };
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v) { return v->mld; }
static bool ieee80211_is_data(uint16_t fc) { return (fc & 12) == 8; }
static bool ieee80211_has_tods(uint16_t fc) { return !!(fc & 0x0100); }
static bool ieee80211_is_action(uint16_t fc) { return (fc & 0xfc) == 0xd0; }
static void ieee80211_free_txskb(void *hw, struct sk_buff *skb) { (void)hw; (void)skb; freed++; }
static int skb_ensure_writable(struct sk_buff *skb, unsigned int length) {
 static struct ieee80211_hdr moved;
 if (!fixture_writable || length!=24) return -1;
 if (fixture_relocate) { memcpy(&moved,skb->data,sizeof(moved));skb->data=(void *)&moved; }
 return 0;
}
static void translate(struct ieee80211_vif *vif, struct ieee80211_tx_control *control,
                      struct ieee80211_tx_info *info, struct mt76_wcid *wcid, struct sk_buff *skb) {
    unsigned link_id;
    void *hw = NULL;
    const struct mt76_driver_ops ops = {.tx_preserve_addr3=mt7925_mlo_tx_preserve_addr3};
    struct mt792x_dev device = {.mt76={.mt7927=fixture_mt7927,.drv=&ops}};
    struct mt792x_dev *dev = &device;
    /* Also compile the pre-fix block, where these inputs are not consumed. */
    (void)dev; (void)mt7925_mlo_preserve_data_da;
    (void)is_mt7927; (void)ieee80211_has_tods;
'''


MAIN = r'''
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const unsigned char final_da[6] = {0x02,0x10,0x20,0x30,0x40,0x50};
    const unsigned char ap_mld[6] = {0x8c,0xed,0xe1,0x98,0xcc,0x18};
    const unsigned char final_sa[6] = {0x02,0x60,0x70,0x80,0x90,0xa0};
    struct ieee80211_bss_conf conf = {.addr={0x02,1,2,3,4,5}, .bssid={0x92,0xed,0xe1,0x98,0xcc,0x1a}};
    struct ieee80211_link_sta peer = {.addr={0x92,0xed,0xe1,0x98,0xcc,0x1a}};
    struct ieee80211_sta sta = {.link={[1]=&peer}};
    struct ieee80211_vif vif = {.type=NL80211_IFTYPE_STATION, .mld=true, .link_conf={[1]=&conf}};
    struct ieee80211_tx_control control = {.sta=&sta};
    struct ieee80211_tx_info info = {0};
    struct mt76_wcid wcid = {.idx=3, .link_id=1};
    struct ieee80211_hdr hdr = {.frame_control=0x0108};
    struct sk_buff skb = {.data=(void *)&hdr, .len=sizeof(hdr)};
    memcpy(hdr.addr3, final_da, 6);
    memcpy(hdr.addr4, final_sa, 6);
    const unsigned char *wanted = final_da;
    const unsigned char zero[6] = {0};
    const unsigned char *wanted_ra = peer.addr, *wanted_ta = conf.addr;
    if (!strcmp(argv[1], "eapol")) { memcpy(hdr.addr3, ap_mld, 6); wanted=ap_mld; }
    else if (!strcmp(argv[1], "four_address")) hdr.frame_control=0x0308;
    else if (!strcmp(argv[1], "relocate")) fixture_relocate=true;
    else if (!strcmp(argv[1], "unwritable")) { fixture_writable=false; wanted_ra=zero;wanted_ta=zero; }
    else if (!strcmp(argv[1], "management")) { hdr.frame_control=0x00b0; wanted=conf.bssid; }
    else if (!strcmp(argv[1], "default_off")) { mt7925_mlo_preserve_data_da=false; wanted=conf.bssid; }
    else if (!strcmp(argv[1], "other_chip")) { fixture_mt7927=false; wanted=conf.bssid; }
    else if (!strcmp(argv[1], "no_ds_data")) { hdr.frame_control=0x0008; wanted=conf.bssid; }
    else if (!strcmp(argv[1], "hardware_encap")) { info.flags=IEEE80211_TX_CTL_HW_80211_ENCAP; wanted_ra=zero; wanted_ta=zero; }
    else if (strcmp(argv[1], "data")) return 2;
    translate(&vif, &control, &info, &wcid, &skb);
    memcpy(&hdr,skb.data,sizeof(hdr));
    bool ok = freed==(!strcmp(argv[1],"unwritable")?1:0) && !memcmp(hdr.addr3, wanted, 6) && !memcmp(hdr.addr4, final_sa, 6)
              && !memcmp(hdr.addr1, wanted_ra, 6) && !memcmp(hdr.addr2, wanted_ta, 6);
    printf("%s: %s addr3=%02x:%02x:%02x:%02x:%02x:%02x expected=%02x:%02x:%02x:%02x:%02x:%02x\n",
           argv[1], ok ? "PASS" : "FAIL", hdr.addr3[0],hdr.addr3[1],hdr.addr3[2],hdr.addr3[3],hdr.addr3[4],hdr.addr3[5],
           wanted[0],wanted[1],wanted[2],wanted[3],wanted[4],wanted[5]);
    return ok ? 0 : 1;
}
'''


class NativeTxHeaders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mt7927-native-header-")
        cls.addClassCleanup(cls.directory.cleanup)
        block = native_block((ROOT / "mt792x_core.c").read_text())
        owner = (ROOT / "mt7925/mlo/tx.c").read_text()
        start = owner.index("bool mt7925_mlo_tx_preserve_addr3(")
        opening = owner.index("{", start)
        depth, end = 1, opening + 1
        while depth:
            depth += (owner[end] == "{") - (owner[end] == "}")
            end += 1
        predicate = owner[start:end]
        prelude = PRELUDE.replace("static void translate(", predicate + "\nstatic void translate(")
        print("Scope: verbatim mt792x_tx native-header block; SHA256=" + hashlib.sha256(block.encode()).hexdigest(), flush=True)
        path = Path(cls.directory.name)
        (path / "fixture.c").write_text(prelude + block + MAIN)
        cls.executable = path / "fixture"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(path / "fixture.c"), "-o", str(cls.executable)], check=True)

    def check_header(self, case):
        result = subprocess.run([str(self.executable), case], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_tods_data_preserves_final_da(self):
        self.check_header("data")

    def test_eapol_data_preserves_ap_mld_da(self):
        self.check_header("eapol")

    def test_four_address_data_preserves_da_and_sa(self):
        self.check_header("four_address")

    def test_management_uses_link_bssid(self):
        self.check_header("management")

    def test_default_off_preserves_previous_behavior(self):
        self.check_header("default_off")

    def test_other_chip_is_unchanged(self):
        self.check_header("other_chip")

    def test_no_ds_data_is_unchanged(self):
        self.check_header("no_ds_data")

    def test_hardware_encap_does_not_mutate_native_headers(self):
        self.check_header("hardware_encap")


    def test_writable_copy_refreshes_header_pointer(self):
        self.check_header("relocate")

    def test_unwritable_header_drops_before_address_mutation(self):
        self.check_header("unwritable")


class AmpduExecution(unittest.TestCase):
    def test_selected_members_use_generic_ba_effects_and_stop_unpins_tid(self):
        """Catch omitted per-member BA state, duplicate MCU sends or stale pins."""
        def function(path, marker):
            source = (ROOT / path).read_text()
            start = source.index(marker)
            opening = source.index("{", start)
            depth, end = 1, opening + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end]

        production = "\n".join([
            function("mt7925/mac.c", "void mt7925_mac_ampdu_link("),
            function("mt7925/mac.c", "int mt7925_mac_ampdu_action("),
            function("mt7925/mlo/tx.c", "void mt7925_mlo_clear_tid_link("),
            function("mt7925/mlo/tx.c", "static const char *mt7925_ampdu_action_name("),
            function("mt7925/mlo/tx.c", "int\nmt7925_mlo_tx_ampdu_action("),
        ])
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint16_t u16;
#define IEEE80211_NUM_TIDS 16
#define IEEE80211_AMPDU_TX_START_IMMEDIATE 42
#define BIT(n) (1UL<<(n))
#define READ_ONCE(x) (x)
#define KERN_INFO ""
#define set_bit(n,p) (*(p)|=BIT(n))
#define clear_bit(n,p) (*(p)&=~BIT(n))
enum ieee80211_ampdu_mlme_action { IEEE80211_AMPDU_RX_START, IEEE80211_AMPDU_RX_STOP, IEEE80211_AMPDU_TX_START, IEEE80211_AMPDU_TX_OPERATIONAL, IEEE80211_AMPDU_TX_STOP_FLUSH, IEEE80211_AMPDU_TX_STOP_FLUSH_CONT, IEEE80211_AMPDU_TX_STOP_CONT };
struct mt76_dev { int x; };
struct mt792x_dev { struct mt76_dev mt76; };
struct mt76_wcid { unsigned int idx; unsigned long ampdu_state; };
struct mt792x_link_sta { struct mt76_wcid wcid; };
struct mt792x_sta { struct mt792x_link_sta *link[3]; bool mlo_tid_link_valid[16]; };
struct ieee80211_txq { void *drv_priv; };
struct ieee80211_link_sta { int x; };
struct ieee80211_sta { void *drv_priv; struct ieee80211_txq *txq[16]; struct ieee80211_link_sta *link[3]; unsigned char addr[6]; };
struct ieee80211_vif { unsigned long active_links; };
struct ieee80211_ampdu_params { enum ieee80211_ampdu_mlme_action action; struct ieee80211_sta *sta; u16 tid,ssn,buf_size; };
struct mt76_txq { bool aggr,send_bar; };
struct ieee80211_hw { struct mt792x_dev *dev; };
static bool mt7925_test_no_tx_ba,mt7925_test_no_rx_ba,mt76_mlo_diag_trace,mt7925_mlo_ctl_trace;
static int locks,rx_start,rx_stop,rx_fw,tx_fw,stopped,logs;
static struct mt792x_dev *mt792x_hw_dev(struct ieee80211_hw *h) { return h->dev; }
static void mt792x_mutex_acquire(struct mt792x_dev *d) { (void)d;locks++; }
static void mt792x_mutex_release(struct mt792x_dev *d) { (void)d;locks--; }
static struct mt792x_link_sta *mt792x_sta_to_link(struct mt792x_sta *s,unsigned int id) { return s->link[id]; }
#define for_each_sta_active_link(v,s,l,i) for ((i)=0;(i)<3;(i)++) if (((v)->active_links&BIT(i)) && ((l)=(s)->link[i]))
static void mt76_rx_aggr_start(struct mt76_dev *d,struct mt76_wcid *w,u16 tid,u16 ssn,u16 size) { (void)d;(void)w;if(tid!=3||ssn!=17||size!=64)rx_start+=100;rx_start++; }
static void mt76_rx_aggr_stop(struct mt76_dev *d,struct mt76_wcid *w,u16 tid) { (void)d;(void)w;if(tid!=3)rx_stop+=100;rx_stop++; }
static void mt7925_mcu_uni_rx_ba(struct mt792x_dev *d,struct ieee80211_ampdu_params *p,struct ieee80211_vif *v,bool on) { (void)d;(void)p;(void)v;rx_fw+=on?1:-1; }
static void mt7925_mcu_uni_tx_ba(struct mt792x_dev *d,struct ieee80211_ampdu_params *p,struct ieee80211_vif *v,bool on) { (void)d;(void)p;(void)v;tx_fw+=on?1:-1; }
static void ieee80211_stop_tx_ba_cb_irqsafe(struct ieee80211_vif *v,unsigned char *a,u16 tid) { (void)v;(void)a;if(tid!=3)stopped+=100;stopped++; }
#define printk(...) (logs+=!!mt7925_ampdu_action_name(action))
"""
        cases = r"""
int main(void) {
 struct testcase { int action,ret,rxstart,rxstop,rxfw,txfw,stop; bool pin,aggr,bar; unsigned long state; } cases[]={
  {0,0,2,0,1,0,0,1,0,1,8}, {1,0,0,2,-1,0,0,1,0,1,8},
  {2,42,0,0,0,0,0,1,0,1,8}, {3,0,0,0,0,1,0,1,1,0,8},
  {4,0,0,0,0,-1,0,0,0,1,0}, {5,0,0,0,0,-1,0,0,0,1,0},
  {6,0,0,0,0,-1,1,0,0,1,0},
 };
 int failures=0;
 for(unsigned int n=0;n<sizeof(cases)/sizeof(cases[0]);n++) {
  struct testcase *t=&cases[n];struct mt792x_dev dev={0};struct ieee80211_hw hw={.dev=&dev};
  struct mt792x_link_sta links[3]={0};struct ieee80211_link_sta ls[3]={0};struct mt792x_sta ms={.mlo_tid_link_valid={[3]=true}};
  struct mt76_txq queue={.send_bar=true};struct ieee80211_txq txq={.drv_priv=&queue};
  struct ieee80211_sta sta={.drv_priv=&ms,.txq={[3]=&txq}};struct ieee80211_vif vif={.active_links=5};
  struct ieee80211_ampdu_params params={.action=t->action,.sta=&sta,.tid=3,.ssn=17,.buf_size=64};
  for(unsigned int id=0;id<3;id++){links[id].wcid.idx=id;links[id].wcid.ampdu_state=t->action==2?0:8;ms.link[id]=&links[id];sta.link[id]=&ls[id];}
  locks=rx_start=rx_stop=rx_fw=tx_fw=stopped=0;
  int ret=mt7925_mlo_tx_ampdu_action(&hw,&vif,&params);
  bool ok=ret==t->ret && locks==0 && rx_start==t->rxstart && rx_stop==t->rxstop && rx_fw==t->rxfw && tx_fw==t->txfw && stopped==t->stop && ms.mlo_tid_link_valid[3]==t->pin && queue.aggr==t->aggr && queue.send_bar==t->bar && links[0].wcid.ampdu_state==t->state && links[2].wcid.ampdu_state==t->state && links[1].wcid.ampdu_state==(t->action==2?0:8);
  printf("%s AMPDU action=%d ret=%d rx=%d/%d MCU=%d/%d stop=%d pin=%d\n",ok?"PASS":"FAIL",t->action,ret,rx_start,rx_stop,rx_fw,tx_fw,stopped,ms.mlo_tid_link_valid[3]);failures+=!ok;
 }
 return !!failures;
}
"""
        with tempfile.TemporaryDirectory(prefix="mlo-ampdu-") as tmp:
            path = Path(tmp)
            (path / "fixture.c").write_text(prefix + production + cases)
            build = subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(path / "fixture.c"), "-o", str(path / "fixture")], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(path / "fixture")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class PnOwnerLiveContext(unittest.TestCase):
    """Execute PCI context selection, not a copied state-machine model."""

    def test_owner_and_live_link_are_independent_with_failure_gates(self):
        source = (ROOT / "mt7925/mlo/tx_context.c").read_text()
        name = "mt7925e_mlo_tx_pn_context("
        self.assertTrue(name in source,
                        "PCI preparation still uses one WCID for both PN and live addressing")
        start = source.index("int\n" + name)
        opening = source.index("{", start)
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        function = source[start:end]
        prelude = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define ETH_ALEN 6
#define IEEE80211_TX_CTL_HW_80211_ENCAP 1
#define IEEE80211_TX_CTRL_PORT_CTRL_PROTO 2
#define IEEE80211_TX_CTL_INJECTED 4
#define IEEE80211_KEY_FLAG_PAIRWISE 1
#define WLAN_CIPHER_SUITE_CCMP 0x000fac04
#define NL80211_IFTYPE_STATION 2
#define MT_TXQ_PSD 4
#define ETH_P_PAE 0x888e
#define cpu_to_be16(x) (x)
#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define READ_ONCE(a) (a)
#define rcu_dereference(a) (a)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define ether_addr_copy(a,b) memcpy(a,b,6)
static bool mt7925e_mlo_tx_pn_owner_live_link = true;
struct ieee80211_hdr { uint16_t frame_control,duration; uint8_t addr1[6],addr2[6],addr3[6]; uint16_t seq; };
struct ieee80211_hdr_3addr {uint8_t bytes[24];};
struct ieee80211_key_conf { int keyidx,flags; unsigned cipher; };
struct ieee80211_tx_info { unsigned flags; struct {unsigned flags; struct ieee80211_key_conf *hw_key;} control; };
struct sk_buff { uint8_t *data; unsigned len,protocol; bool cloned,shared; struct ieee80211_tx_info info; };
#define IEEE80211_SKB_CB(s) (&(s)->info)
struct mt76_wcid { unsigned idx,link_id; int hw_key_idx; bool sta,link_valid; };
struct mt76_vif_link { unsigned idx,omac_idx,wmm_idx,band_idx; };
struct ieee80211_bss_conf { uint8_t addr[6]; const uint8_t *bssid; };
struct ieee80211_vif { int type; bool mld; unsigned active_links; struct ieee80211_bss_conf *link_conf[16]; };
struct mt792x_bss_conf {struct mt76_vif_link mt76;};
struct mt792x_vif {struct mt792x_bss_conf *link_conf[16];};
struct mt792x_sta;
struct mt792x_link_sta {struct mt76_wcid wcid; struct mt792x_sta *sta;};
struct mt792x_sta {struct mt792x_link_sta deflink; struct mt792x_link_sta *link[16]; struct mt792x_vif *vif;};
struct ieee80211_sta {bool tdls; _Alignas(8) uint8_t drv_priv[sizeof(struct mt792x_sta)];};
struct mt76_dev {bool mt7927,mmio; struct mt76_wcid *wcid[32];};
struct mt7925e_mlo_tx_context {struct mt76_wcid *owner; uint8_t omac,wmm,band,ra[6],ta[6];};
static bool is_mt7927(struct mt76_dev *d){return d->mt7927;}
static bool mt76_is_mmio(struct mt76_dev *d){return d->mmio;}
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v){return v->mld;}
static bool ieee80211_is_data_present(uint16_t f){return (f&0x4c)==8;}
static bool ieee80211_has_tods(uint16_t f){return f&0x100;}
static bool ieee80211_has_fromds(uint16_t f){return f&0x200;}
static bool ieee80211_has_protected(uint16_t f){return f&0x4000;}
static bool is_multicast_ether_addr(uint8_t *a){return a[0]&1;}
static bool skb_header_cloned(struct sk_buff *s){return s->cloned;}
static bool skb_shared(struct sk_buff *s){return s->shared;}
struct joined_vif {struct ieee80211_vif vif; struct mt792x_vif private;};
#define vif_from_private(p) (&container_of(p,struct joined_vif,private)->vif)
'''
        # Kernel container_of uses drv_priv; fixture keeps identical ownership
        # with explicit containing storage rather than allocating kernel vifs.
        function = function.replace(
            "container_of((void *)msta->vif, struct ieee80211_vif, drv_priv)",
            "vif_from_private(msta->vif)")
        source = (ROOT / "mt7925/pci_mac.c").read_text()
        begin = source.index("\tif (split) {\n\t\tstruct ieee80211_hdr *hdr")
        opening = source.index("{", begin)
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        descriptor_block = source[opening:end]
        prelude = prelude.replace("bool mt7927,mmio;", "bool mt7927,mmio; void *dma_dev;")
        descriptor_prelude = r'''
typedef uint32_t __le32;
typedef uint32_t u32;
#define le32_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define FIELD_PREP(mask,value) (((uint32_t)(value)<<__builtin_ctz(mask)) & (mask))
#define MT_TXD0_Q_IDX 0xfe000000U
#define MT_TXD1_OWN_MAC 0x7e000000U
#define MT_TXD1_TGID 0x00003000U
#define MT_TXD3_HW_AMSDU 0x20U
#define MT_TXD3_BA_DISABLE 0x10000000U
#define MT_TXD5_FL 0x8000U
#define MT_TXD6_DIS_MAT 8U
#define MT76_CONNAC_MAX_WMM_SETS 4
#define DMA_TO_DEVICE 1
struct mt76_tx_info {struct sk_buff *skb; struct {unsigned long addr,len;} buf[2];};
static int sync_stage;
static void dma_sync_single_for_cpu(void *d,unsigned long a,unsigned long l,int dir) {
 (void)d; assert(a==123 && l==sizeof(struct ieee80211_hdr) && dir==1 && sync_stage==0); sync_stage=1;
}
static void dma_sync_single_for_device(void *d,unsigned long a,unsigned long l,int dir) {
 (void)d; assert(a==123 && l==sizeof(struct ieee80211_hdr) && dir==1 && sync_stage==1); sync_stage=2;
}
static unsigned skb_get_queue_mapping(struct sk_buff *s) {(void)s;return 2;}
static unsigned mt76_connac_lmac_mapping(unsigned q) {return q;}
static void apply(struct mt76_dev *mdev,struct mt76_tx_info *tx_info,void *txwi_ptr,struct mt7925e_mlo_tx_context context)
'''
        body = r'''
int main(void) {
 struct mt76_dev d={.mt7927=true,.mmio=true};
 struct ieee80211_sta s={}; struct mt792x_sta *m=(void *)s.drv_priv;
 struct joined_vif v={.vif={.type=2,.mld=true,.active_links=3}};
 struct mt792x_link_sta live={.wcid={.idx=3,.link_id=1,.hw_key_idx=0,.sta=true,.link_valid=true},.sta=m};
 struct mt792x_bss_conf b={.mt76={.idx=2,.omac_idx=4,.wmm_idx=1,.band_idx=1}};
 struct ieee80211_bss_conf c={.addr={2,1,2,3,4,5},.bssid=(uint8_t[]){0x9a,0xed,0xe1,0x98,0xcc,0x1a}};
 struct ieee80211_key_conf k={.keyidx=0,.flags=1,.cipher=WLAN_CIPHER_SUITE_CCMP};
 struct ieee80211_hdr h={.frame_control=0x4108,.addr1={0x8e,0xed,0xe1,0x98,0xcc,0x18},.addr3={2,3,4,5,6,7}};
 struct sk_buff skb={.data=(void *)&h,.len=sizeof(h),.info={.control={.hw_key=&k}}};
 struct mt7925e_mlo_tx_context out={};
 m->vif=&v.private; m->deflink.sta=m;
 m->deflink.wcid=(struct mt76_wcid){.idx=1,.link_id=2,.hw_key_idx=0,.sta=true};
 d.wcid[1]=&m->deflink.wcid; d.wcid[3]=&live.wcid;
 m->link[1]=&live; v.private.link_conf[1]=&b; v.vif.link_conf[1]=&c;
 assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)==1);
 assert(out.owner->idx==1 && out.owner->link_id==2 && out.omac==4 && out.wmm==1 && out.band==1);
 assert(!memcmp(out.ra,c.bssid,6) && !memcmp(out.ta,c.addr,6));
 assert(h.addr3[0]==2 && h.addr1[0]==0x8e); /* selector never edits skb */
 {
  uint32_t txd[8]={0x01000064,1,0,0x22,0xdeadbeef,0xbeef0003,4,0};
  struct mt76_tx_info ti={.skb=&skb,.buf={[1]={.addr=123,.len=sizeof(h)}}};
  apply(&d,&ti,txd,out);
  assert(sync_stage==2 && !memcmp(h.addr1,c.bssid,6) && !memcmp(h.addr2,c.addr,6) && h.addr3[0]==2);
  assert((txd[1]&0xfff)==1 && (txd[1]&0x7e000000)==0x08000000 && (txd[1]&0x3000)==0x1000);
  assert(txd[0]==0x0d000064 && txd[3]==0x10000002);
  assert(txd[4]==0xdeadbeef && txd[5]==0xbeef8003 && txd[6]==12);
 }
 mt7925e_mlo_tx_pn_owner_live_link=false;
 assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)==0);
 mt7925e_mlo_tx_pn_owner_live_link=true;
 d.mt7927=false; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)==0); d.mt7927=true;
 skb.info.flags=1; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)==0); skb.info.flags=0;
 skb.info.control.flags=2; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)==0); skb.info.control.flags=0;
 skb.protocol=ETH_P_PAE; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)==0); skb.protocol=0;
 skb.info.flags=4; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)==0); skb.info.flags=0;
 h.frame_control=0x00b0; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)==0); h.frame_control=0x4108;
 d.wcid[1]=0; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)<0); d.wcid[1]=&m->deflink.wcid;
 m->deflink.wcid.hw_key_idx=-1; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)<0); m->deflink.wcid.hw_key_idx=0;
 v.vif.active_links=1; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)<0); v.vif.active_links=3;
 v.private.link_conf[1]=0; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)<0); v.private.link_conf[1]=&b;
 skb.cloned=true; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)<0);
 skb.cloned=false; skb.shared=true; assert(mt7925e_mlo_tx_pn_context(&d,&s,&skb,&live.wcid,0,&out)<0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="mt7927-pn-context-") as directory:
            path = Path(directory)
            (path / "test.c").write_text(prelude + function + descriptor_prelude + descriptor_block + body)
            executable = path / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.c"), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


class PeerAssociationFailures(unittest.TestCase):
    def test_real_assoc_stops_and_returns_firmware_failure(self):
        """Ignored BSS/STA failure must not look like completed admission.

        Execute the real helper, replacing only hardware/MCU boundaries.
        Legacy void ABI is called normally and has no result to propagate.
        This tests failure handling, not PN residency or firmware acceptance.
        """
        source = (ROOT / "mt7925/main.c").read_text()
        name = "mt7925_mac_link_sta_assoc(struct mt792x_dev *dev,"
        start = source.index(name, source.index("/* Mutex-free core"))
        opening = source.index("{", start)
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        legacy_void = source[start-5:start].strip() == "void"
        function = ("void " if legacy_void else "int ") + source[start:end]
        lifecycle = (ROOT / "mt7925/mlo/lifecycle.c").read_text()
        retry_start = lifecycle.index("struct mt7927_mlo_admit_retry_ctx {")
        retry_end = lifecycle.index("\nstatic void mt7927_mlo_rb_trace", retry_start)
        retry_function = lifecycle[retry_start:retry_end]
        invoke = ("mt7925_mac_link_sta_assoc(&dev,&vif,&link); ret=0;"
                  if legacy_void else "ret=mt7925_mac_link_sta_assoc(&dev,&vif,&link);")
        prelude = r'''
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define NL80211_IFTYPE_STATION 2
#define MT76_STA_INFO_STATE_ASSOC 2
#define MT_WTBL_UPDATE_ADM_COUNT_CLEAR 1
#define KERN_INFO ""
#define READ_ONCE(x) (x)
#define printk(...) printf(__VA_ARGS__)
#define MT76_STA_INFO_STATE_NONE 0
static bool mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
struct mt792x_link_sta { struct { unsigned idx, phy_idx, hw_key_idx, link_valid; } wcid; int avg_ack_signal, airtime_ac[4]; };
struct mt792x_sta { struct mt792x_link_sta peer; unsigned long mlo_link_ready_jiffies[16]; };
struct mt792x_vif { struct { unsigned transition_generation, migration_from_links, migration_to_links; } mt7927_mlo; };
struct ieee80211_sta { void *drv_priv; bool tdls; };
struct ieee80211_link_sta { struct ieee80211_sta *sta; unsigned link_id; };
struct ieee80211_bss_conf { unsigned link_id; };
struct ieee80211_vif { int type; bool mld; struct ieee80211_bss_conf bss_conf; void *drv_priv; };
struct mt792x_bss_conf { struct { void *ctx; } mt76; };
struct mt792x_dev { int phy; };
static struct ieee80211_bss_conf conf;
static struct mt792x_bss_conf mconf;
static int bss_error, sta_error, bss_calls, sta_calls, wtbl_calls;
static unsigned resolved_link;
static bool ieee80211_vif_is_mld(struct ieee80211_vif *v) { return v->mld; }
static struct mt792x_link_sta *mt792x_sta_to_link(struct mt792x_sta *s,unsigned id)
{ (void)id; return &s->peer; }
static struct ieee80211_bss_conf *mt792x_vif_to_bss_conf(void *v,unsigned id)
{ (void)v; resolved_link=id; return &conf; }
static struct mt792x_bss_conf *mt792x_link_conf_to_mconf(void *c) { (void)c; return &mconf; }
static int mt7925_mcu_add_bss_info(void *p,void *c,void *b,void *l,bool e)
{ (void)p;(void)c;(void)b;(void)l;(void)e;bss_calls++;return bss_error; }
static int mt7925_mcu_sta_update(void *d,void *l,void *v,void *m,bool e,int s,void *pending)
{ (void)d;(void)l;(void)v;(void)m;(void)e;(void)s;sta_calls++;return sta_error; }
static int mt7925_mlo_reconcile_link(void *d,void *v,void *s,void *m,unsigned id)
{ (void)d;(void)v;(void)s;(void)m;(void)id;return 0; }
static void ewma_avg_signal_init(int *v) { *v=0; }
static void mt7925_mac_wtbl_update(void *d,int i,int f)
{ (void)d;(void)i;(void)f;wtbl_calls++; }
static int wake_calls;
static int mt7927_mlo_link_wake_probe(void *d,void *v,void *s,void *p,unsigned id)
{ (void)d;(void)v;(void)s;(void)p;(void)id;wake_calls++;return 0; }
'''
        main = r'''
int main(void) {
 struct mt792x_dev dev={0}; struct mt792x_sta msta={0};
 struct ieee80211_sta sta={.drv_priv=&msta};
 struct ieee80211_link_sta link={.sta=&sta,.link_id=1};
 struct mt792x_vif mvif={0};
 struct ieee80211_vif vif={.type=2,.mld=true,.drv_priv=&mvif};
 const int errors[][2]={{-EIO,0},{0,-ETIMEDOUT},{0,0},{0,0}};
 int failures=0;
 for(unsigned i=0;i<4;i++) {
  bss_error=errors[i][0];sta_error=errors[i][1];
  bss_calls=sta_calls=wtbl_calls=0;
  int ret;
  INVOKE
  int want=i==0 ? -EIO : i==1 ? -ETIMEDOUT : 0;
  if(ret!=want || bss_calls!=1 || sta_calls!=(i==0 ? 0:1) ||
     wtbl_calls!=(i==0 ? 0:1) || resolved_link!=1) {
   fprintf(stderr,"case %u ret=%d want=%d bss=%d sta=%d wtbl=%d\n",
    i,ret,want,bss_calls,sta_calls,wtbl_calls);failures++;
  }
 }
 struct mt7927_mlo_admit_retry_ctx ctx={.dev=&dev,.vif=&vif,
  .sta=&sta,.msta=&msta,.link_sta=&link,.peer=&msta.peer};
 for(unsigned i=0;i<2;i++) {
  bss_error=i==0 ? -EIO:0;sta_error=i==1 ? -ETIMEDOUT:0;
  int ret=mt7927_mlo_reauthorize_added_link(&ctx,1,true);
  int want=i==0 ? -EIO:-ETIMEDOUT;
  if(ret!=want) { fprintf(stderr,"retry %u ret=%d want=%d\n",i,ret,want);failures++; }
 }
 if(wake_calls!=2) { fprintf(stderr,"wake probe calls=%d want 2 (once per retry)\n",wake_calls);failures++; }
 printf("association MCU failure/retry: 6 cases, %d failures\n",failures);
 return !!failures;
}
'''.replace("INVOKE", invoke)
        with tempfile.TemporaryDirectory(prefix="mt7927-assoc-result-") as directory:
            c = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            c.write_text(prelude + function + retry_function + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            str(c), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class NativeRxIdentity(unittest.TestCase):
    """Real parser extraction: header geometry, identity, bounds, no skb edits."""
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mt7927-rx-identity-")
        cls.addClassCleanup(cls.tmp.cleanup)
        source = (ROOT / "mt7925/mlo/datapath.c").read_text()
        if "/* MLO_RX_DIAG_BEGIN */" in source:
            header = (ROOT / "mt7925/mlo/datapath.h").read_text()
            struct_start = header.index("struct mt7925_mlo_rx_diag {")
            struct_end = header.index("\n};", struct_start) + len("\n};")
            code = header[struct_start:struct_end] + source.split("/* MLO_RX_DIAG_BEGIN */")[1].split("/* MLO_RX_DIAG_END */")[0]
        else:
            old = (ROOT / "mt76.h").read_text()
            code = RX_STRUCT + old[old.index("static inline void mt76_mlo_skb_pkt_sig("):old.index("static inline void\nmt76_mlo_tx_select_set_pkt_sig(")] + r'''
static void mt7925_mlo_rx_diag_parse(struct sk_buff *s,u32 flags,u8 sec,bool amsdu,struct mt7925_mlo_rx_diag *d) {
 (void)flags;(void)sec;(void)amsdu;memset(d,0,sizeof(*d));mt76_mlo_skb_pkt_sig(s,&d->sig);
}
'''
        path = Path(cls.tmp.name)
        (path / "fixture.c").write_text(RX_PRELUDE + code + RX_MAIN)
        cls.exe = path / "fixture"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                        str(path / "fixture.c"), "-o", str(cls.exe)], check=True)
        print("Verbatim RX parser SHA256=" + hashlib.sha256(code.encode()).hexdigest(), flush=True)

    def parse(self, frame, flags=3, sec=4, amsdu=False, head=None):
        result = subprocess.run([str(self.exe), frame.hex() or "-", str(flags), str(sec),
                                 str(int(amsdu)), str(len(frame) if head is None else head)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return {k: v if k in ("sa", "da") else int(v) for k, v in
                (field.split("=", 1) for field in result.stdout.split())}

    @staticmethod
    def ip(payload=bytes.fromhex("00000000532e003d"), proto=1, fragment=0, options=b"", total=None):
        size = 20 + len(options)
        return struct.pack("!BBHHHBBH4s4s", 0x40 + size // 4, 0,
                           size + len(payload) if total is None else total, 0x1234,
                           fragment, 64, proto, 0, b"\x0a\x07\x0c\x34", b"\x0a\x07\x0c\x2d") + options + payload

    @staticmethod
    def native(ip, fc=0x4a88, seq=0x400, qos=5):
        frame = struct.pack("<HH", fc, 0) + bytes(range(0x10, 0x16)) + bytes.fromhex("9aede198cc1a") + bytes(range(0x30, 0x36)) + struct.pack("<H", seq)
        if fc & 0x300 == 0x300:
            frame += bytes(range(0x40, 0x46))
        if fc & 0x80:
            frame += struct.pack("<H", qos) + (b"\0" * 4 if fc & 0x8000 else b"")
        return frame + bytes.fromhex("aaaa030000000800") + ip

    def test_retained_native_geometry_not_ethernet(self):
        row = self.parse(self.native(self.ip()))
        self.assertEqual((row["l3"], row["l4"], row["id"], row["seq"]), (0x800, 1, 21294, 61))
        self.assertEqual((row["sa"], row["da"]), ("303132333435", "101112131415"))

    def test_ethernet_and_vlan_and_length_snap(self):
        for header in (b"\x08\x00", b"\x81\x00\x00\x01\x08\x00", b"\x00\x24" + bytes.fromhex("aaaa030000000800")):
            self.assertEqual(self.parse(bytes(range(12)) + header + self.ip(), flags=7)["id"], 21294)

    def test_ds_geometry_qos_ht_and_nonlinear(self):
        for fc, sa, da in ((0x4088, "9aede198cc1a", "101112131415"), (0x4188, "9aede198cc1a", "303132333435"),
                           (0x4288, "303132333435", "101112131415"), (0xc388, "404142434445", "303132333435")):
            for head in (0, 1, 13, 24, 31):
                row = self.parse(self.native(self.ip(), fc=fc), head=head)
                self.assertEqual((row["id"], row["sa"], row["da"]), (21294, sa, da), (fc, head))

    def test_every_short_prefix_is_unknown_not_out_of_bounds(self):
        frame = self.native(self.ip())
        for n in range(len(frame)):
            row = self.parse(frame[:n], head=min(n, 9))
            self.assertEqual((row["id"], row["l4_valid"]), (0, 0), n)

    def test_declared_8023_length_excludes_padding_identity(self):
        # Valid-looking SNAP/IP in padding is not part of the length envelope.
        for length in (0, 1, 7, 8, 20, 35):
            for vlan in (b"", bytes.fromhex("81000001")):
                frame = bytes(range(12)) + vlan + struct.pack("!H", length) + bytes.fromhex("aaaa030000000800") + self.ip()
                self.assertEqual(self.parse(frame, flags=7)["l4_valid"], 0, (length, vlan))

    def test_fragments_amsdu_null_ciphertext_bad_snap(self):
        good = self.native(self.ip())
        cases = [(self.native(self.ip(), fc=0x4688), 3, False), (self.native(self.ip(), seq=0x401), 3, False),
                 (self.native(self.ip(fragment=0x2000)), 3, False), (self.native(self.ip(fragment=1)), 3, False),
                 (good, 3, True), (self.native(self.ip(), qos=0x85), 3, False), (self.native(self.ip(), fc=0x42c8), 3, False),
                 (good[:26] + b"\0" * 8 + good[34:], 3, False), (good, 0, False), (good, 11, False)]
        for frame, flags, amsdu in cases:
            self.assertEqual(self.parse(frame, flags=flags, amsdu=amsdu)["l4_valid"], 0)

    def test_ccmp_iv_and_ip_options_and_tcp_identity(self):
        frame = self.native(self.ip(options=b"\1" * 4))
        self.assertEqual(self.parse(frame[:26] + b"\1\2\3\x20\4\5\6\7" + frame[26:], flags=1)["id"], 21294)
        tcp = struct.pack("!HHIIBBHHH", 39337, 5201, 0x12345678, 0x76543210, 0x50, 0x18, 4096, 0, 0)
        row = self.parse(self.native(self.ip(tcp, proto=6)))
        self.assertEqual((row["sport"], row["dport"], row["tcp_seq"], row["tcp_ack"], row["l4_valid"]), (39337, 5201, 0x12345678, 0x76543210, 1))
        self.assertEqual(self.parse(self.native(self.ip(tcp[:19], proto=6)))["l4_valid"], 0)


RX_STRUCT = r'''
struct mt7925_mlo_rx_diag {
 struct mt76_mlo_pkt_sig sig; __be32 src_ip,dst_ip; u32 tcp_seq,tcp_ack;
 u16 sport,dport,ip_id,fc,seq_ctrl; u8 sa[6],da[6],ra[6],ta[6],tid;
 bool mac_valid,ip_valid,l4_valid,truncated;
};
'''
RX_PRELUDE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/if_arp.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
#define RX_FLAG_DECRYPTED 1
#define RX_FLAG_IV_STRIPPED 2
#define RX_FLAG_8023 4
#define RX_FLAG_ONLY_MONITOR 8
#define RX_FLAG_FAILED_FCS_CRC 16
#define RX_FLAG_MMIC_ERROR 32
#define MT_CIPHER_AES_CCMP 4
#define MT_CIPHER_CCMP_CCX 9
#define MT_CIPHER_CCMP_256 10
#define MT_CIPHER_GCMP 11
#define MT_CIPHER_GCMP_256 12
#define IEEE80211_FCTL_VERS 3
#define IEEE80211_SCTL_FRAG 15
#define IEEE80211_QOS_CTL_A_MSDU_PRESENT 0x80
#define IEEE80211_QOS_CTL_MESH_CONTROL_PRESENT 0x100
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define ether_addr_copy(a,b) memcpy(a,b,6)
#define cpu_to_le16(x) (x)
#define le16_to_cpu(x) (x)
struct sk_buff {u8 *data,*frag;unsigned len,head_len;};
static void *skb_header_pointer(struct sk_buff *s,int o,int n,void *scratch) {
 if(o<0 || n<0 || (unsigned)o>s->len || (unsigned)n>s->len-(unsigned)o)return NULL;
 if((unsigned)o+(unsigned)n<=s->head_len)return s->data+o;
 for(int i=0;i<n;i++)((u8 *)scratch)[i]=(unsigned)(o+i)<s->head_len?s->data[o+i]:s->frag[o+i-s->head_len];
 return scratch;
}
static u16 get_unaligned_be16(const void *p){const u8 *b=p;return (b[0]<<8)|b[1];}
static u32 get_unaligned_be32(const void *p){const u8 *b=p;return ((u32)b[0]<<24)|((u32)b[1]<<16)|(b[2]<<8)|b[3];}
struct ieee80211_hdr {__le16 frame_control,duration_id;u8 addr1[6],addr2[6],addr3[6];__le16 seq_ctrl;u8 addr4[6];} __attribute__((packed));
static bool ieee80211_is_data_present(u16 f){return (f&0x4c)==8;}
static bool ieee80211_is_data_qos(u16 f){return (f&0x8c)==0x88;}
static bool ieee80211_has_a4(u16 f){return (f&0x300)==0x300;}
static bool ieee80211_has_morefrags(u16 f){return f&0x400;}
static bool ieee80211_has_protected(u16 f){return f&0x4000;}
static unsigned ieee80211_hdrlen(u16 f){return 24+(ieee80211_has_a4(f)?6:0)+(ieee80211_is_data_qos(f)?2+((f&0x8000)?4:0):0);}
static u8 *ieee80211_get_SA(struct ieee80211_hdr *h){return ieee80211_has_a4(h->frame_control)?h->addr4:(h->frame_control&0x200)?h->addr3:h->addr2;}
static u8 *ieee80211_get_DA(struct ieee80211_hdr *h){return h->frame_control&0x100?h->addr3:h->addr1;}
struct mt76_mlo_pkt_sig {u16 l3_proto,arp_op,icmp_id,icmp_seq;u8 l4_proto,icmp_type,icmp_code;};
'''
RX_MAIN = r'''
int main(int argc,char **argv) {
 assert(argc==6);size_t n=!strcmp(argv[1],"-")?0:strlen(argv[1])/2;
 u8 *bytes=calloc(n+1,1),*before=calloc(n+1,1);assert(bytes && before);
 for(size_t i=0;i<n;i++){unsigned v;assert(sscanf(argv[1]+2*i,"%2x",&v)==1);bytes[i]=v;}
 memcpy(before,bytes,n);unsigned head=strtoul(argv[5],NULL,0);assert(head<=n);
 struct sk_buff s={.data=bytes,.frag=bytes+head,.len=n,.head_len=head};struct mt7925_mlo_rx_diag d;
 mt7925_mlo_rx_diag_parse(&s,strtoul(argv[2],NULL,0),strtoul(argv[3],NULL,0),atoi(argv[4]),&d);
 assert(!memcmp(bytes,before,n) && s.len==n && s.head_len==head);
 printf("l3=%u l4=%u id=%u seq=%u sport=%u dport=%u tcp_seq=%u tcp_ack=%u l4_valid=%u truncated=%u sa=",d.sig.l3_proto,d.sig.l4_proto,d.sig.icmp_id,d.sig.icmp_seq,d.sport,d.dport,d.tcp_seq,d.tcp_ack,d.l4_valid,d.truncated);
 for(int i=0;i<6;i++) { printf("%02x",d.sa[i]); }
 printf(" da="); for(int i=0;i<6;i++) { printf("%02x",d.da[i]); } puts("");
 free(bytes);free(before);return 0;
}
'''


if __name__ == "__main__":
    unittest.main(verbosity=2)
