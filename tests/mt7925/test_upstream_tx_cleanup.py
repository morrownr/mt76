#!/usr/bin/env python3
"""Exercise upstream TX metadata, WCID teardown locking, and removed-device reset."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(path, name):
    text = (ROOT / path).read_text()
    start = re.search(r"^(?:static )?void\s+" + re.escape(name) + r"\(", text, re.M).start()
    begin = text.index("{", start)
    depth, end = 1, begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class UpstreamTxCleanupTest(unittest.TestCase):
    def test_rx_pn_security_unchanged_without_mt7922_mlo_telemetry(self):
        actual = function("mac80211.c", "mt76_check_ccmp_pn")
        call = ("mt76_check_ccmp_pn(&dev,&skb);" if "struct mt76_dev *dev" in actual
                else "mt76_check_ccmp_pn(&skb);")
        prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define RX_FLAG_DECRYPTED 1
#define RX_FLAG_ONLY_MONITOR 2
#define RX_FLAG_8023 4
#define RX_FLAG_IV_STRIPPED 8
#define RX_FLAG_PN_VALIDATED 16
#define IEEE80211_QOS_CTL_TID_MASK 15
#define IEEE80211_NUM_TIDS 16
#define READ_ONCE(x) (x)
#define unlikely(x) (x)
#define BUILD_BUG_ON(x) _Static_assert(!(x), "size")
#define KERN_INFO ""
struct mt76_dev {unsigned chip;};
#define mt76_chip(d) ((d)->chip)
struct mt76_wcid {unsigned idx,link_id;bool rx_check_pn;unsigned char rx_key_pn[17][6];};
struct mt76_rx_status {unsigned flag,qos_ctl;struct mt76_wcid *wcid;unsigned char iv[6];};
struct ieee80211_hdr {unsigned frame_control,seq_ctrl;};
struct sk_buff {struct mt76_rx_status *cb;struct ieee80211_hdr hdr;};
static int logs,mt76_mlo_diag_trace=1;
#define printk(...) (++logs)
static struct ieee80211_hdr *mt76_skb_get_hdr(struct sk_buff *s){return &s->hdr;}
static bool ieee80211_is_frag(struct ieee80211_hdr *h){return h->seq_ctrl&15;}
static bool ieee80211_is_first_frag(unsigned seq){return !(seq&15);}
static bool ieee80211_is_mgmt(unsigned fc){return !(fc&12);}
static bool ieee80211_has_tods(unsigned fc){return fc&256;}
'''
        main = r'''
int main(void){
 for(int chip=0;chip<2;chip++) for(int replay=0;replay<2;replay++) {
  struct mt76_dev dev={.chip=chip?0x7927:0x7922};
  struct mt76_wcid wc={.idx=7,.link_id=0,.rx_check_pn=true};
  struct mt76_rx_status status={.flag=RX_FLAG_DECRYPTED|RX_FLAG_IV_STRIPPED|RX_FLAG_8023,.wcid=&wc};
  struct sk_buff skb={.cb=&status};
  wc.rx_key_pn[0][5]=5;status.iv[5]=replay?4:6;logs=0;
  CALL
  if(logs!=(chip?1:0) ||
     !!(status.flag&RX_FLAG_ONLY_MONITOR)!=replay ||
     !!(status.flag&RX_FLAG_PN_VALIDATED)==replay ||
     wc.rx_key_pn[0][5]!=(replay?5:6)) {
   fprintf(stderr,"chip=%x replay=%d logs=%d flags=%u stored=%u\n",dev.chip,replay,logs,status.flag,wc.rx_key_pn[0][5]);return 1;
  }
 }
 puts("PASS actual PN enforcement on both chips; MLO telemetry only MT7927");return 0;
}
'''.replace('CALL', call)
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'pn.c', Path(tmp) / 'pn'
            src.write_text(prefix + actual + main)
            build = subprocess.run(['cc', '-Wall', '-Werror', '-Wno-unused-value',
                                    str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_auto_band_does_not_encode_tgid_and_teardown_is_locked(self):
        tx = (ROOT / "mt7925/mac.c").read_text()
        start = tx.index("val = FIELD_PREP(MT_TXD1_WLAN_IDX")
        end = tx.index("txwi[2] = 0;", start)
        tx = tx[start:end]
        header = (ROOT / "mt76_connac3_mac.h").read_text()
        masks = "\n".join(re.findall(r"^#define MT_TXD1_(?:WLAN_IDX|OWN_MAC|TGID)\s+.*$", header, re.M))
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define FIELD_PREP(mask,val) (((uint32_t)(val)<<__builtin_ctz(mask)) & (mask))
#define cpu_to_le32(v) (v)
struct list_head { int unused; };
struct sk_buff_head { int lock; };
struct sk_buff { int unused; };
struct ieee80211_hw { int unused; };
struct mt76_phy { int tx_lock; unsigned long state; };
struct mt76_dev { struct mt76_phy phy; int sta_poll_lock,wq; };
struct mt76_wcid { int idx,phy_idx,pktid; struct list_head poll_list,tx_list; struct sk_buff_head tx_pending,tx_offchannel; };
struct mt76_connac_pm { bool suspended; };
struct mt792x_dev { struct mt76_dev mt76; struct mt76_connac_pm pm; bool hw_init_done,hw_full_reset; int reset_work; };
#define container_of(p,t,m) ((t *)(p))
#define MT76_REMOVED 3
#define test_bit(b,p) ((*(p) >> (b)) & 1UL)
#define spin_lock_bh(p) ((void)(p))
#define spin_unlock_bh(p) ((void)(p))
#define spin_lock(p) ((void)(p))
#define spin_unlock(p) ((void)(p))
static int status_locked,lock_failure,destroys,queued;
static struct mt76_phy *mt76_dev_phy(struct mt76_dev *d,int idx) { return &d->phy; }
static void mt76_tx_status_lock(struct mt76_dev *d,struct sk_buff_head *h) { status_locked=1; }
static void mt76_tx_status_skb_get(struct mt76_dev *d,struct mt76_wcid *w,int id,struct sk_buff_head *h) { }
static void mt76_tx_status_unlock(struct mt76_dev *d,struct sk_buff_head *h) { status_locked=0; }
static void idr_destroy(int *p) { destroys++; if (!status_locked) lock_failure++; }
static int list_empty(struct list_head *l) { return 1; }
static void list_del_init(struct list_head *l) { }
static void skb_queue_splice_tail_init(struct sk_buff_head *a,struct sk_buff_head *b) { }
static struct sk_buff *__skb_dequeue(struct sk_buff_head *h) { return NULL; }
static struct ieee80211_hw *mt76_tx_status_get_hw(struct mt76_dev *d,struct sk_buff *s) { return NULL; }
static void ieee80211_free_txskb(struct ieee80211_hw *h,struct sk_buff *s) { }
static void queue_work(int wq,int *work) { queued++; }
"""
        wrapper = "\nstatic uint32_t encode(int band_idx) { uint32_t val,txwi[3]; int omac_idx=3; struct mt76_wcid wc={.idx=5},*wcid=&wc;\n" + tx + "return txwi[1]; }\n"
        suffix = r"""
int main(void) {
 int bands[]={0,1,2,255}; uint32_t wants[]={0x06000005,0x06001005,0x06002005,0x06000005};
 int failures=0;
 for (int i=0;i<4;i++) if (encode(bands[i])!=wants[i]) { failures++; fprintf(stderr,"band=%d encoded=%x expected=%x\n",bands[i],encode(bands[i]),wants[i]); }
 struct mt76_dev dev={0};struct mt76_wcid wc={0};mt76_wcid_cleanup(&dev,&wc);
 if (destroys!=1 || lock_failure || status_locked) { failures++;fprintf(stderr,"destroy count=%d outside lock=%d\n",destroys,lock_failure); }
 struct mt792x_dev chip={.hw_init_done=true};
 for (int scenario=0;scenario<5;scenario++) {
  chip.hw_init_done=scenario!=1;chip.hw_full_reset=scenario==2;chip.pm.suspended=scenario==3;
  chip.mt76.phy.state=scenario==4?1UL<<MT76_REMOVED:0;queued=0;
  mt792x_reset(&chip.mt76);
  if (queued!=(scenario==0)) { failures++;fprintf(stderr,"reset scenario=%d queued=%d\n",scenario,queued); }
 }
 printf("10 checks, %d failures\n",failures);return failures?1:0;
}
"""
        source = prefix + masks + wrapper + function("mac80211.c", "mt76_wcid_cleanup") + "\n" + function("mt792x_mac.c", "mt792x_reset") + suffix
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "tx.c", Path(tmp) / "tx"
            src.write_text(source)
            build = subprocess.run(["cc", "-Wall", "-Werror", str(src), "-o", str(exe)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
