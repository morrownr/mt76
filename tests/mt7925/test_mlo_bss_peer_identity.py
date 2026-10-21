#!/usr/bin/env python3
"""Execute the production band move with MCU boundaries replaced by recorders.

Regressions caught: using a BMC identity for a PHY move, recreating the
unchanged initial peer before the incoming STA exists, and losing a failed
move on retry. Firmware boundaries are explicit doubles, not firmware proof.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

root = Path(__file__).resolve().parents[2]
source = (root / 'mt7925/mlo/lifecycle.c').read_text()
start = source.index('mt7927_mlo_reassign_band(')
end = source.index('\nstatic int\nmt7927_mlo_reassign_active_bands', start)
function = 'static int\n' + source[start:end]
start = source.index('static int\nmt7927_mlo_refresh_reassigned_sta(')
end = source.index('\nstatic int\nmt7927_mlo_add_bss_link(', start)
refresh = source[start:end]
# Supplement the full real functions with their actual incarnation-boundary
# tails. These are not a replacement for the real mac80211/core fixture.
start = source.index('\nerr_remove_created:', source.index('int mt7927_mlo_change_vif_links('))
end = source.index('\n}\n', start)
unwind = ('static int unwind_created(struct mt792x_dev *dev, struct mt792x_vif *mvif, '
          'unsigned long created, int err) { unsigned int link_id;\n' +
          source[start + len('\nerr_remove_created:'):end] + '\n}\n')
main_source = (root / 'mt7925/mlo/association.c').read_text()
end = main_source.index('\nout_pm:', main_source.index('static int mt7925_mac_link_sta_add('))
start = main_source.rindex('\n\tdev_info(', 0, end)
created = ('static int fresh_peer_created(struct mt792x_dev *dev, struct mt792x_vif *mvif, '
           'struct mt792x_sta *msta, unsigned int link_id) { (void)dev;(void)mvif;\n' +
           main_source[start:end] + '\n}\n')
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define IEEE80211_MLD_MAX_NUM_LINKS 16
#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define for_each_set_bit(i,p,n) for ((i)=0; (i)<(n); (i)++) if ((*(p)) & BIT(i))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define rcu_assign_pointer(p,v) ((p)=(v))
#define dev_info(...) ((void)0)
#define KERN_INFO ""
#define MT76_STA_INFO_STATE_NONE 0
static bool mt7925_test_mt7927_migrate_rebuild;
static bool mt76_mlo_diag_trace, mt7925_mlo_ctl_trace;
static bool mt7925_mlo_restore_peer_valid;
static bool mt7925_mlo_skip_unmoved_rebuild __attribute__((unused));
static bool mt7925_mlo_sync_peer_phy __attribute__((unused));
#define printk(...) ((void)0)
struct ieee80211_sta { int marker; };
struct ieee80211_link_sta { struct ieee80211_sta *sta; };
struct ieee80211_bss_conf { int marker; };
struct ieee80211_vif { void *drv_priv; };
struct wcid { bool link_valid; u8 phy_idx, hw_key_idx, hw_key_idx2; int idx;
 struct { u32 tx_packets; } stats; };
struct mt792x_link_sta { struct wcid wcid; u32 mlo_valid_rx_packets;
 unsigned long mlo_valid_rx_jiffies, tx_liveness_rx_progress_jiffies;
 u32 tx_liveness_rx_seen, tx_liveness_tx_seen; bool tx_liveness_rx_ever_seen; };
struct mt792x_sta { struct mt792x_link_sta deflink;
 struct mt792x_link_sta *link[16];
 unsigned long mlo_link_ready_jiffies[16]; };
struct mt792x_bss_conf { struct { u8 band_idx; void *ctx; } mt76; };
struct mt792x_vif {
 struct mt792x_sta sta, *wep_sta;
 struct mt792x_bss_conf bss_conf, *link_conf[16];
 u16 valid_links;
 struct { u16 resident_links; struct { int fw_context_slot; bool band_moved; } link[16]; } mt7927_mlo;
};
struct mt792x_dev { int pm, mphy, phy; struct { void *dev; } mt76; };
static inline bool is_mt7927(void *m) { (void)m; return true; }
static void mt792x_mutex_release(void *d) { (void)d; }
static void mt792x_mac_link_bss_remove(void *d,void *b,void *m) { (void)d;(void)b;(void)m; }
static void devm_kfree(void *d,void *p) { (void)d;(void)p; }
static struct mt792x_bss_conf bss;
static struct mt792x_link_sta bmc, peer;
static struct mt792x_sta peer_owner;
static struct ieee80211_bss_conf conf;
static struct ieee80211_sta sta;
static struct ieee80211_link_sta link = { &sta };
static bool have_peer, missing_sta, missing_link;
static unsigned peer_link_id;
static int bss_calls, bss_error, restore_calls, dev_calls, drain_calls;
static int recreate_calls, recreate_error;
static unsigned long jiffies=100;
static struct ieee80211_link_sta *emitted_peer;
static struct mt792x_bss_conf *mt7927_mlo_resident_bss(void *d, void *v, unsigned id)
{ (void)d; (void)v; (void)id; return &bss; }
static struct mt792x_link_sta *mt7927_mlo_resident_sta(void *d, struct mt792x_sta *s, unsigned id)
{ (void)d; return s == &peer_owner ? (have_peer && id==peer_link_id ? &peer : NULL) : &bmc; }
static struct ieee80211_bss_conf *mt792x_vif_to_bss_conf(void *v, unsigned id)
{ (void)v; (void)id; return &conf; }
static inline struct ieee80211_sta *wcid_to_sta(struct wcid *w)
{ (void)w; return missing_sta ? NULL : &sta; }
static inline struct ieee80211_link_sta *mt792x_sta_to_link_sta(void *v, void *s, unsigned id)
{ (void)v; (void)s; (void)id; return missing_link ? NULL : &link; }
static void mt76_connac_free_pending_tx_skbs(void *p, void *w) { (void)p; (void)w; drain_calls++; }
static int mt76_connac_mcu_uni_add_dev(void *p, void *c, void *b, void *w, bool e)
{ (void)p; (void)c; (void)b; (void)w; (void)e; dev_calls++; return 0; }
static int mt7925_mcu_add_bss_info(void *p, void *c, void *b, struct ieee80211_link_sta *l, bool e)
{ (void)p; (void)c; (void)b; (void)e; bss_calls++; emitted_peer=l; return bss_error; }
/* The production helper has an optional peer removal before migration.
 * These fixtures exercise the default path, with that experiment off.
 */
static int mt7925_mcu_sta_update(void *d, void *l, void *v, void *p, bool e, int s, void *pending)
{ (void)d; (void)l; (void)v; (void)p; (void)e; (void)s; (void)pending; return 0; }
static int mt7925_mlo_group_key_rebind(void *d, void *v, unsigned id, void *x)
{ (void)d; (void)v; (void)id; (void)x; restore_calls++; return 0; }
/* External firmware/key boundary: count requested peer programming and
 * return a controlled failure. Never simulate PN, WTBL or authenticated RX. */
static int mt7927_mlo_recreate_authorize_peer(void *d,void *v,void *s,void *m,
 void *l,struct mt792x_link_sta *p,unsigned id,bool retry,bool defer)
{ (void)d;(void)v;(void)s;(void)m;(void)l;(void)id;(void)retry;(void)defer;
 recreate_calls++; if(recreate_error)return recreate_error;
 p->wcid.link_valid=true;return 0; }
static void msleep(unsigned ms) { (void)ms; }
'''
suffix = r'''
int main(void)
{
 /* Literal fixture PHY moves: link0->slot0; non-link0 survivor->slot1
  * when link0 joins; link1->slot0 when link0 leaves. */
 const unsigned pairs[][5] = {
  {6,5,2,1,1}, {5,3,0,0,0}, {3,6,1,1,0},
  {6,3,1,0,1}, {3,5,0,0,0}, {5,6,2,1,1}
 };
 struct mt792x_dev dev={0}; struct ieee80211_vif vif={0};
 struct mt792x_vif mvif={0}; int failures=0, checks=0;
 for (unsigned i=0; i<6; i++) {
  unsigned old=pairs[i][3], next=pairs[i][4];
  if (old==next) continue; /* No destructive survivor move in this direction. */
  bss.mt76.band_idx=old; bmc.wcid.phy_idx=old; bmc.wcid.link_valid=true;
  peer.wcid.phy_idx=old; peer.wcid.link_valid=true;
  have_peer=true; peer_link_id=pairs[i][2]; mvif.wep_sta=&peer_owner;
  bss_calls=restore_calls=bss_error=0; emitted_peer=NULL;
  int ret=mt7927_mlo_reassign_band(&dev,&vif,&mvif,pairs[i][2],next);
  checks++;
  if (ret || bss_calls!=1 || emitted_peer!=&link) {
   fprintf(stderr,"FAIL 0x%x->0x%x survivor=%u: BSS must carry real peer link_sta (ret=%d calls=%d peer=%s)\n",
    pairs[i][0],pairs[i][1],pairs[i][2],ret,bss_calls,emitted_peer ? "present" : "NULL/BMC"); failures++;
  }
 }
 /* Band-changing link with no resident peer remains a safe BMC-only BSS. */
 bss.mt76.band_idx=0; have_peer=false; mvif.wep_sta=NULL;
 bss_calls=restore_calls=bss_error=0; emitted_peer=&link;
 int ret=mt7927_mlo_reassign_band(&dev,&vif,&mvif,0,1); checks++;
 if(ret || bss_calls!=1 || emitted_peer) { puts("FAIL absent peer"); failures++; }
 /* A rejected BSS update cannot be published as successful migration. */
 bss.mt76.band_idx=0; bmc.wcid.phy_idx=0; bss_error=-EIO;
 restore_calls=0;
 ret=mt7927_mlo_reassign_band(&dev,&vif,&mvif,0,1); checks++;
 if(ret!=-EIO || restore_calls || bss.mt76.band_idx!=0) { puts("FAIL BSS error propagation"); failures++; }
 /* Existing peer with unresolved mac80211 identity must fail before
  * changing host eligibility, draining TX, or issuing firmware writes. */
 for (unsigned missing=0; missing<2; missing++) {
  have_peer=true; peer_link_id=1; mvif.wep_sta=&peer_owner;
  missing_sta=missing==0; missing_link=missing==1;
  bss.mt76.band_idx=0; bmc.wcid.phy_idx=0;
  bmc.wcid.link_valid=peer.wcid.link_valid=true;
  bss_error=bss_calls=dev_calls=restore_calls=drain_calls=0;
  ret=mt7927_mlo_reassign_band(&dev,&vif,&mvif,1,1); checks++;
  if(ret!=-EINVAL || bss_calls || dev_calls || restore_calls || drain_calls ||
     bss.mt76.band_idx!=0 || bmc.wcid.phy_idx!=0 ||
     !bmc.wcid.link_valid || !peer.wcid.link_valid) {
   fprintf(stderr,"FAIL missing %s must reject before side effects\n",missing ? "link_sta" : "sta");
   failures++;
  }
 }
 /* Initial VIFgrow precedes incoming STA allocation. There is only an
  * unchanged L2 peer on BSS band1, but its creation-time phy_idx is0.
  * Recreating it would authorize active6 before peer1 exists (the live
  * serializer returned-EINVAL at this boundary). Preserve all its proof. */
 missing_sta=missing_link=false;have_peer=true;peer_link_id=2;
 vif.drv_priv=&mvif;mvif.wep_sta=&peer_owner;
 bss.mt76.band_idx=1;mvif.mt7927_mlo.link[2].band_moved=false;
 peer.wcid.phy_idx=0;peer.wcid.hw_key_idx=0;peer.wcid.link_valid=true;
 peer.mlo_valid_rx_packets=99;peer.mlo_valid_rx_jiffies=55;
 peer_owner.mlo_link_ready_jiffies[2]=77;
 recreate_calls=0;recreate_error=-EINVAL;
 ret=mt7927_mlo_refresh_reassigned_sta(&dev,&vif,&sta,&peer_owner,6,false);checks++;
 if(ret || recreate_calls || peer.wcid.phy_idx!=0 ||
    peer.mlo_valid_rx_jiffies!=55 || peer_owner.mlo_link_ready_jiffies[2]!=77) {
  fprintf(stderr,"FAIL initial unchanged L2: ret=%d peer_commands=%d rx_ts=%lu ready=%lu\n",
   ret,recreate_calls,peer.mlo_valid_rx_jiffies,peer_owner.mlo_link_ready_jiffies[2]);failures++;
 }
 /* A real acknowledged DEV/BSS move must complete even when the old
  * bookkeeping accidentally equals the destination. Failures must retain
  * the pending move; the second call must retry, not report a false no-op. */
 const int errors[]={-EIO,-EKEYREJECTED,-EINVAL};
 for(unsigned same=0;same<2;same++) for(unsigned e=0;e<3;e++) {
  mvif.mt7927_mlo.link[2].band_moved=true;
  peer.wcid.phy_idx=same ? 1 : 0;
  peer.wcid.hw_key_idx=255;peer.wcid.link_valid=false;
  peer_owner.mlo_link_ready_jiffies[2]=77;
  recreate_calls=0;recreate_error=errors[e];
  ret=mt7927_mlo_refresh_reassigned_sta(&dev,&vif,&sta,&peer_owner,4,false);checks++;
  if(ret!=errors[e] || recreate_calls!=1 ||
     !mvif.mt7927_mlo.link[2].band_moved ||
     peer_owner.mlo_link_ready_jiffies[2]!=0) {
   fprintf(stderr,"FAIL pending move error=%d same_phy=%u ret=%d commands=%d pending=%d ready=%lu\n",
    errors[e],same,ret,recreate_calls,mvif.mt7927_mlo.link[2].band_moved,
    peer_owner.mlo_link_ready_jiffies[2]);failures++;
  }
  recreate_error=0;
  ret=mt7927_mlo_refresh_reassigned_sta(&dev,&vif,&sta,&peer_owner,4,false);checks++;
  if(ret || recreate_calls!=2 || mvif.mt7927_mlo.link[2].band_moved ||
     peer.wcid.phy_idx!=1 || !peer.wcid.link_valid ||
     peer_owner.mlo_link_ready_jiffies[2]!=100) {
   fprintf(stderr,"FAIL retry move same_phy=%u ret=%d commands=%d pending=%d ready=%lu\n",
    same,ret,recreate_calls,mvif.mt7927_mlo.link[2].band_moved,
    peer_owner.mlo_link_ready_jiffies[2]);failures++;
  }
  ret=mt7927_mlo_refresh_reassigned_sta(&dev,&vif,&sta,&peer_owner,4,false);checks++;
  if(ret || recreate_calls!=2) { puts("FAIL completed move repeated");failures++; }
 }
 /* A BMC-only move cannot arm a rebind for a peer that did not exist. */
 bss.mt76.band_idx=0;have_peer=false;mvif.wep_sta=NULL;
 mvif.mt7927_mlo.link[2].band_moved=false;bss_error=0;
 ret=mt7927_mlo_reassign_band(&dev,&vif,&mvif,2,1);checks++;
 if(ret || mvif.mt7927_mlo.link[2].band_moved) {
  puts("FAIL BMC-only move arms future peer recreation");failures++;
 }
 /* Successful new-peer programming consumes an older incarnation's
  * pending marker. The extracted tail is reached only on success. */
 mvif.mt7927_mlo.link[2].band_moved=true;
 ret=fresh_peer_created(&dev,&mvif,&peer_owner,2);checks++;
 if(ret || mvif.mt7927_mlo.link[2].band_moved ||
    peer_owner.mlo_link_ready_jiffies[2]!=100) {
  puts("FAIL fresh peer inherits older pending refresh");failures++;
 }
 /* Failed grow discards only objects it created, including their marker. */
 mvif.mt7927_mlo.link[1].band_moved=true;
 mvif.mt7927_mlo.link[2].band_moved=true;
 mvif.valid_links=mvif.mt7927_mlo.resident_links=6;
 mvif.link_conf[1]=&bss;mvif.sta.link[1]=&bmc;
 ret=unwind_created(&dev,&mvif,2,-EIO);checks++;
 if(ret!=-EIO || mvif.mt7927_mlo.link[1].band_moved ||
    !mvif.mt7927_mlo.link[2].band_moved || mvif.valid_links!=4 ||
    mvif.mt7927_mlo.resident_links!=4 || mvif.link_conf[1] || mvif.sta.link[1]) {
  puts("FAIL failed-grow cleanup must discard only created-link pending state");failures++;
 }
 printf("%d checks, %d failures\n",checks,failures);
 return failures ? 1 : 0;
}
'''
class BssPeerIdentityTest(unittest.TestCase):
    def test_actual_band_move_and_peer_refresh_contract(self):
        with tempfile.TemporaryDirectory(prefix='.bss-peer-test-', dir=root) as temp:
            c = Path(temp) / 'test.c'
            binary = Path(temp) / 'test'
            production_header = '#include "' + str(root / 'mt7925/mlo_peer_move.h') + '"\n'
            c.write_text(prefix + production_header + function + refresh + created + unwind + suffix)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(binary)], check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            print(run.stdout, end='')
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
