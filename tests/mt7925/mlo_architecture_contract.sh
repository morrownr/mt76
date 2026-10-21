#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# MLO logic stays inside mt7925/mlo/. Generic files only carry the minimal
# integration boundary (the NIC_CAP TLV loop calling in, one phy field).

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail() { printf 'mlo-architecture contract failed: %s\n' "$1" >&2; exit 1; }
fn() { sed -n "/^$1(/,/^}/p" "$2"; }

# firmware MLO capability: decode + debugfs live in mlo/, mcu.c only calls in
grep -q 'mlo_fw_caps' mt7925/debugfs.c && fail "generic debugfs.c registers an MLO capability file"
grep -q 'struct mt7925_mcu_mlo_cap' mt7925/mcu.c && fail "generic mcu.c decodes the MLO capability record"
grep -q 'mt7925_mlo_fw_cap_parse(dev, tlv->data' mt7925/mcu.c || fail "mcu.c does not hand the NIC_CAP record to mlo/"
d=$(fn 'void mt7925_mlo_fw_cap_parse' mt7925/mlo/mlo.c)
[[ -n $d ]] || fail "mlo/mlo.c has no capability decoder"
grep -q 'nonap_eml_support' <<< "$d" || fail "decoder drops the support byte"
grep -q 'if (len < sizeof' <<< "$d" || fail "decoder has no length check"
grep -q 'mt7925_mlo_fw_cap_parse' mt7925/mlo/mlo.h || fail "decoder is not declared in mlo.h"
grep -q '"mlo_fw_caps"' mt7925/mlo/pairs.c || fail "capability readout is not registered by the MLO debugfs init"

# TTLM request: one helper, owned by ttlm.c, used by the steering controller and
# the test-only control; no second copy of the parameter construction
t=$(fn 'int mt7925_mlo_ttlm_request' mt7925/mlo/ttlm.c)
[[ -n $t ]] || fail "ttlm.c has no request helper"
grep -q 'ieee80211_request_neg_ttlm' <<< "$t" || fail "helper does not use the mac80211 negotiation API"
grep -q 'mt7925_mlo_ttlm_mapping_valid' <<< "$t" || fail "helper does not validate the mapping"
grep -q 'ieee80211_request_neg_ttlm(vif' mt7925/mlo/steering.c && fail "steering.c still builds its own TTLM request"
grep -q 'mt7925_mlo_ttlm_request(' mt7925/mlo/steering.c || fail "steering.c does not use the helper"
grep -q '"mlo_ttlm_request"' mt7925/mlo/pairs.c || fail "TTLM test control is not registered by the MLO debugfs init"
grep -q 'mt7925_mlo_ttlm_request(' mt7925/mlo/pairs.c || fail "TTLM control does not use the helper"

printf 'mlo-architecture contract: PASS\n'
