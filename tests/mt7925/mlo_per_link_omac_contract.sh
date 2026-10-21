#!/bin/sh
set -eu

python3 - <<'PY'
import re
from pathlib import Path

main = Path("mt7925/mlo/association.c").read_text()

assignment = re.compile(
    r"mconf->mt76\.omac_idx = ieee80211_vif_is_mld\(vif\)\s*\?\s*"
    r"0\s*:\s*mconf->mt76\.idx;"
)
if not assignment.search(main):
    raise SystemExit("FAIL: affiliated MLD links do not share OMAC 0")

if re.search(r"mconf->mt76\.omac_idx\s*=\s*mconf->mt76\.idx;", main):
    raise SystemExit("FAIL: MLD links still bind independent OMAC identities")

print("PASS: affiliated MLD links share their hardware OMAC identity")
PY
