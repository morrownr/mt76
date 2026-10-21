#!/usr/bin/env python3
"""Execute generic ROC callers against a different, matching, or absent owner."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
def function(path, name):
    text = (ROOT / path).read_text()
    start = text.index(name + "(")
    start = text.rfind("\n", 0, start) + 1
    begin = text.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

class RocOwnerTest(unittest.TestCase):
    def test_cancel_and_cleanup_preserve_another_interfaces_roc(self):
        prefix = r"""
#include <stdio.h>
struct ieee80211_vif;
struct mt76_phy { struct ieee80211_vif *roc_vif; };
struct mt76_vif_data { struct mt76_phy *roc_phy; void *link[1]; };
struct mt76_vif_link { struct mt76_vif_data *mvif; };
struct ieee80211_vif { struct mt76_vif_link drv_priv[1]; };
struct ieee80211_hw { int unused; };
struct mt76_dev { int unused; };
#define rcu_assign_pointer(p,v) ((p)=(v))
static int aborts;
static void mt76_abort_roc(struct mt76_phy *p) { aborts++; }
static void mt76_abort_scan(struct mt76_dev *d) { }
"""
        suffix = r"""
int main(void) {
 struct ieee80211_hw hw={0}; struct mt76_dev dev={0};
 struct mt76_vif_data data={0}; struct ieee80211_vif vif={{{&data}}}, other={0};
 struct mt76_phy phy={&other}; data.roc_phy=&phy;
 int failures=0, checks=0;
 for (int caller=0;caller<2;caller++) {
  for (int scenario=0;scenario<3;scenario++) {
   data.roc_phy=scenario==2?NULL:&phy;
   phy.roc_vif=scenario==0?&other:&vif; aborts=0;
   if (caller) mt76_vif_cleanup(&dev,&vif);
   else mt76_cancel_remain_on_channel(&hw,&vif);
   checks++; int expected=scenario==1;
   if (aborts!=expected) { failures++; fprintf(stderr,"caller=%d scenario=%d aborted=%d expected=%d\n",caller,scenario,aborts,expected); }
  }
 }
 printf("%d checks, %d failures\n",checks,failures); return failures?1:0;
}
"""
        source = prefix + "\n" + function("channel.c", "mt76_cancel_remain_on_channel") + "\n" + function("mac80211.c", "mt76_vif_cleanup") + suffix
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/"roc.c"; exe=Path(tmp)/"roc";src.write_text(source)
            compiled=subprocess.run(["cc","-Wall","-Werror",str(src),"-o",str(exe)],text=True,capture_output=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            result=subprocess.run([str(exe)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__ == "__main__": unittest.main()
