#!/usr/bin/env python3
"""Compile the real MLD-only sender and builder against command recorders."""
from pathlib import Path
import ast
import subprocess
import tempfile

import unittest

def run_fixture(root=None):
    root = Path(root) if root is not None else Path(__file__).resolve().parents[2]
    source = (root/'mt7925/mlo/peer.c').read_text()
    def function(name):
        start = source.index(name+'(')
        opening = source.index('{', start)
        depth, end = 1, opening+1
        while depth:
            depth += (source[end]=='{')-(source[end]=='}')
            end += 1
        return source[start:end]

    if 'int mt7925_mcu_mld_presetup(' not in source:
        raise AssertionError('standalone MLD presetup sender is missing')
    # Reuse the neighboring actual-builder harness ABI, without executing it.
    tree = ast.parse(Path(__file__).with_name('test_mlo_setup_wlan.py').read_text())
    prefix = next(ast.literal_eval(n.value) for n in ast.walk(tree)
                  if isinstance(n, ast.Assign) and any(
                      isinstance(t, ast.Name) and t.id=='prefix' for t in n.targets))
    prefix = prefix.replace('struct sk_buff { struct sta_rec_mld record; bool emitted; };',
        'struct sk_buff { struct sta_rec_mld record; bool emitted; unsigned len, wcid, bss; };')
    prefix = prefix.replace('} mt76; };', '} mt76; int mphy; };')
    prefix = prefix.replace('u8 idx; } mt76;', 'u8 idx; } mt76;')
    prefix = prefix.replace('s->emitted=true;', 's->len+=len; s->emitted=true;')
    stubs = r'''
    #include <errno.h>
    #include <stdlib.h>
    #define MCU_UNI_CMD(x) 0x20003
    #define STA_REC_UPDATE 3
    #define IS_ERR(p) ((intptr_t)(p)<0)
    #define PTR_ERR(p) ((int)(intptr_t)(p))
    #define dev_kfree_skb(p) free(p)
    #define pr_info(...) ((void)0)
    #define print_hex_dump(...) ((void)0)
    struct sta_req_hdr { char data[8]; };
    static bool mt7925_test_mld_presetup;
    static unsigned allocations, sends, fail_alloc, fail_send;
    static struct sk_buff commands[2];
    static unsigned hweight16(unsigned long x) { return __builtin_popcount(x); }
    static unsigned __ffs(unsigned long x) { return __builtin_ctzl(x); }
    static struct sk_buff *__mt76_connac_mcu_alloc_sta_req(void *d, void *b, void *w, unsigned len)
    {
     struct { u8 idx; } *bss=b;
     struct { unsigned idx, link_id; bool link_valid; } *wcid=w;
     (void)d; (void)len;
     if (++allocations==fail_alloc) return (void *)(intptr_t)-ENOMEM;
     struct sk_buff *s=calloc(1,sizeof(*s));
     s->len=sizeof(struct sta_req_hdr); s->wcid=wcid->idx; s->bss=bss->idx;
     return s;
    }
    static int mt76_mcu_skb_send_msg(void *d, struct sk_buff *s, unsigned cmd, bool wait)
    {
     (void)d;
     if (cmd!=0x20003 || !wait) abort();
     commands[sends++]=*s; free(s);
     return sends==fail_send ? -EIO : 0;
    }
    '''
    suffix = r'''
    int main(void)
    {
     struct mt792x_dev dev={.mt76={.chip7927=true}};
     struct mt792x_phy phy={.dev=&dev};
     struct mt792x_vif mvif={.phy=&phy};
     struct mt792x_sta msta={.deflink_id=2};
     struct ieee80211_vif vif={.drv_priv=&mvif,.active_links=4};
     struct ieee80211_sta sta={.drv_priv=&msta,.addr={0x8c,0xed,0xe1,0x98,0xcc,0x18}};
     struct mt792x_bss_conf bss[3]; struct mt792x_link_sta peer[3];
     unsigned failures=0,checks=0;
     const unsigned ids[]={3,2,1};
     for(unsigned i=0;i<3;i++) {
      bss[i].link_id=i; bss[i].mt76.idx=i;
      peer[i].wcid.idx=ids[i]; peer[i].wcid.link_id=i; peer[i].wcid.link_valid=true;
      mvif.link[i]=&bss[i]; msta.link[i]=&peer[i];
     }
    #define CHECK(x) do { checks++; if(!(x)) { failures++; printf("FAIL line %d\n",__LINE__); } } while(0)
     CHECK(mt7925_mcu_mld_presetup(&dev,&vif,&sta,0)==0 && sends==0);
     mt7925_test_mld_presetup=true; dev.mt76.chip7927=false;
     CHECK(mt7925_mcu_mld_presetup(&dev,&vif,&sta,0)==0 && sends==0);
     dev.mt76.chip7927=true; vif.active_links=7;
     CHECK(mt7925_mcu_mld_presetup(&dev,&vif,&sta,0)==-EINVAL && sends==0);
     vif.active_links=4;
     CHECK(mt7925_mcu_mld_presetup(&dev,&vif,&sta,0)==0 && sends==2);
     CHECK(commands[0].wcid==1 && commands[1].wcid==3);
     for(unsigned i=0;i<2;i++) {
      CHECK(commands[i].len==8+28 && commands[i].record.tag==0x20 && commands[i].record.len==28);
      CHECK(commands[i].record.link_num==2 && commands[i].record.primary_id==1 && commands[i].record.secondary_id==3);
      CHECK(commands[i].record.link[0].wlan_id==1 && commands[i].record.link[1].wlan_id==3);
     }
     CHECK(mvif.mt7927_mlo.rebind_target_links==0 && vif.active_links==4);
     for(unsigned which=1;which<=2;which++) {
      allocations=sends=0; fail_alloc=which;
      CHECK(mt7925_mcu_mld_presetup(&dev,&vif,&sta,0)==-ENOMEM && sends==which-1 && mvif.mt7927_mlo.rebind_target_links==0);
      fail_alloc=0; allocations=sends=0; fail_send=which;
      CHECK(mt7925_mcu_mld_presetup(&dev,&vif,&sta,0)==-EIO && sends==which && mvif.mt7927_mlo.rebind_target_links==0);
      fail_send=0;
     }
     allocations=sends=0; mvif.link[0]=NULL;
     CHECK(mt7925_mcu_mld_presetup(&dev,&vif,&sta,0)==-EINVAL && sends==0);
     mvif.link[0]=&bss[0]; peer[0].wcid.link_valid=false;
     CHECK(mt7925_mcu_mld_presetup(&dev,&vif,&sta,0)==-EINVAL && sends==0);
     peer[0].wcid.link_valid=true;
     /* BSS lookup succeeds but refers to another link: the real builder must
      * reject before a firmware command, and the sender must restore override.
      */
     bss[0].link_id=1; allocations=sends=0;
     CHECK(mt7925_mcu_mld_presetup(&dev,&vif,&sta,0)==-EINVAL && sends==0 &&
           allocations==1 && mvif.mt7927_mlo.rebind_target_links==0);
     printf("%u checks, %u failures\n",checks,failures); return failures!=0;
    }
    '''
    with tempfile.TemporaryDirectory() as temp:
        p=Path(temp)/'test.c'
        p.write_text(prefix+stubs+'static int\n'+function('mt7925_mcu_sta_mld_tlv')+
                     '\nint '+function('mt7925_mcu_mld_presetup')+suffix)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra',str(p),'-o',str(p.with_suffix(''))],check=True)
        subprocess.run([str(p.with_suffix(''))],check=True)


class ProductionFixture(unittest.TestCase):
    def test_real_mld_presetup_sender(self):
        run_fixture()


if __name__ == "__main__":
    unittest.main()
