#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Steering policy v2 contract: candidates are policy MODES resolved to the
# negotiated band->link map; EMLSR modes are unavailable unless every
# prerequisite is real; the single-radio 5+6 GHz bitmap is never a candidate;
# a current mask that is not a mode does not pin the selector.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail() { printf 'policy-modes contract failed: %s\n' "$1" >&2; exit 1; }
fn() { sed -n "/^$1(/,/^}/p" "$2"; }

main=mt7925/mlo/steering.c

[[ -f mt7925/mlo_policy_modes.h ]] || fail "pure mode table missing"
gcc -Wall -Wextra -Werror -I mt7925 -o /tmp/test_mlo_policy_modes.$$ \
	mt7925/test_mlo_policy_modes.c
/tmp/test_mlo_policy_modes.$$ > /dev/null || fail "unit test fails"
rm -f /tmp/test_mlo_policy_modes.$$

grep -q '#include "../mlo_policy_modes.h"' $main || fail "controller does not use the mode table"

# the old bitmap candidate array (which carried 0x6) is gone
grep -q 'mt7925_mlo_steer_pairs\[\]' $main && fail "bitmap candidate array still present"
t=$(sed -n '/^static const u8 mt7925_mlo_steer_modes/,/^};/p' $main)
[[ -n $t ]] || fail "mode candidate table missing"
for m in SIM_24_5 SIM_24_6 EMLSR_5 EMLSR_6; do
	grep -q "MT7925_MLO_MODE_$m" <<< "$t" || fail "mode $m not a candidate"
done

# no firmware switch primitive is claimed without evidence
p=$(fn 'static bool mt7925_mlo_emlsr_switch_primitive_verified' $main)
grep -q 'return false;' <<< "$p" || fail "switch primitive is claimed verified without evidence"

# the selector evaluates availability before scoring and reports it
w=$(fn 'void mt7925_mlo_steer_work' $main)
grep -q 'mt7925_mlo_build_mode_ctx(' <<< "$w" || fail "selector builds no mode context"
grep -q 'mt7925_mlo_mode_eval(' <<< "$w" || fail "selector does not evaluate mode availability"
grep -q 'mt7925_mlo_note_mode_avail(' <<< "$w" || fail "mode availability is not reported"
grep -q 'avail != MT7925_MLO_MODE_OK' <<< "$w" || fail "unavailable mode can still be scored"
grep -q 'mt7925_mlo_mode_of_links(current_links' <<< "$w" || fail "current mask is not classified"
grep -q '!cur_is_mode' <<< "$w" || fail "non-mode current mask still pins selection"

# the context uses discovered bands and the AP's own EML capability
b=$(fn 'void mt7925_mlo_build_mode_ctx' $main)
grep -q 'mt7925_mlo_link_band(vif, link_id)' <<< "$b" || fail "band map is not discovered"
grep -q 'vif->cfg.eml_cap' <<< "$b" || fail "AP EML capability not read from the association"
grep -q 'cfg80211_get_iftype_ext_capa' <<< "$b" || fail "local EML advertisement not read"

# failure backoff: a failed target waits out its own backoff, a commit proves it
grep -q 'mt7927_mlo_pair_backoff_active(vif, pair)' <<< "$w" || fail "selector ignores the failure backoff"
v=$(fn 'static void mt7925_mlo_steer_switch_validate' $main)
grep -q 'mt7927_mlo_pairs_note(vif, pending_pair, 0, "commit", true)' <<< "$v" || fail "commit is not recorded for the target mode"
grep -q 'mt7927_mlo_pairs_note(vif, pending_pair, -EIO, "validation_failed", false)' <<< "$v" || fail "failed validation does not feed the backoff"
c=$(grep -n 'MLO_STEER_COMMIT' <<< "$v" | head -1 | cut -d: -f1)
n=$(grep -n '"commit", true' <<< "$v" | head -1 | cut -d: -f1)
[[ -n $c && -n $n && $n -gt $c ]] || fail "commit is recorded before validation passed"

# a transition class is attempted only once verified in this association
grep -q '^static bool mt7925_mlo_steer_try_unverified;' $main || fail "unverified attempts are not default-off"
grep -q 'mt7925_mlo_switch_feasible(cls,' <<< "$w" || fail "selector does not gate on transition class"
grep -q 'verified_switch_classes' <<< "$w" || fail "selector ignores which classes are verified"
grep -q 'MLO_STEER_MODE_BLOCKED' <<< "$w" || fail "blocked transition is not reported"
grep -q 'verified_switch_classes |=' <<< "$v" || fail "a validated commit does not verify its class"
vn=$(grep -n 'verified_switch_classes |=' <<< "$v" | head -1 | cut -d: -f1)
cn=$(grep -n 'MLO_STEER_COMMIT' <<< "$v" | head -1 | cut -d: -f1)
[[ -n $vn && -n $cn && $vn -gt $cn ]] || fail "class is verified before the commit"
e=$(fn 'static void mt7925_mlo_steer_epoch_sync' $main)
grep -q 'verified_switch_classes = 0' <<< "$e" || fail "verification outlives its association"

# a rollback never "restores" a mask that is not a steering mode
grep -q 'prev_is_mode = mt7925_mlo_mode_of_links(prev_pair, &mctx) != 0' <<< "$v" || fail "rollback does not classify the previous mask"
[[ $(grep -c 'goto rollback_skipped' <<< "$v") -ge 2 ]] || fail "both rollback paths must skip a non-mode previous mask"
grep -q 'MLO_STEER_ROLLBACK_SKIPPED' <<< "$v" || fail "skipped rollback is not reported"
sk=$(grep -n 'goto rollback_skipped' <<< "$v" | tail -1 | cut -d: -f1)
rq=$(grep -n 'req_ret = mt7925_mlo_steer_request(dev, vif, prev_pair' <<< "$v" | tail -1 | cut -d: -f1)
[[ -n $sk && -n $rq && $sk -lt $rq ]] || fail "the skip must precede the rollback request"

# autonomy: starts by itself on authorization, behind the steering gate, no debugfs
pairs=mt7925/mlo/pairs.c
a=$(fn 'void mt7927_mlo_pairs_authorized' $pairs)
grep -q 'mt7927_mlo_profiles_can_steer(&priv->profiles)' <<< "$a" || fail "autostart bypasses the steering gate"
grep -q 'mlo_auto_steering, true' <<< "$a" || fail "authorization does not start steering"
k=$(fn 'void mt7927_mlo_steering_kick' $main)
grep -q 'mt7927_mlo_pairs_autostart(vif)' <<< "$k" || fail "foreign-event stop is never re-armed"
r=$(fn 'void mt7927_mlo_pairs_autostart' $pairs)
grep -q 'auto_intent' <<< "$r" || fail "autostart ignores whether steering is wanted"
grep -q 'mt7927_mlo_profiles_can_steer(&priv->profiles)' <<< "$r" || fail "re-arm bypasses the steering gate"
i=$(fn 'void mt7927_mlo_pairs_invalidate' $pairs)
grep -q 'auto_intent = false' <<< "$i" || fail "security loss does not cancel the steering intent"
grep -q 'priv->auto_intent = !!value' $pairs || fail "operator stop is not honoured"
grep -q 'mt7927_mlo_steering_kick(vif)' mt7925/mlo/association.c || fail "authorization does not schedule the controller"

# the steering gate no longer demands the single-radio 5+6 GHz profile
g=$(fn 'bool mt7927_mlo_profiles_can_steer' $pairs)
grep -q 'i < 2' <<< "$g" || fail "gate still counts the 5+6 GHz profile"
grep -q 'current_validated' <<< "$g" && fail "gate depends on a validation nothing can set before steering"

# firmware EML capability comes from the decoded MLO capability record, not from
# what the driver advertises
grep -q 'mlo_fw_cap.nonap_eml_support' <<< "$b" || fail "firmware non-AP EML support flag is not consulted"
grep -q 'c->local_emlsr_adv =' <<< "$b" || fail "local advertisement is not separate from the firmware report"
grep -q 'c->fw_emlsr = c->local' <<< "$b" && fail "firmware claim is derived from our own advertisement"
mc=$(fn 'void mt7925_mlo_fw_cap_parse' mt7925/mlo/mlo.c)
grep -q 'nonap_eml_support' <<< "$mc" || fail "CAP_MLO_CAP support byte is not decoded"
grep -q 'max_simul_links' <<< "$mc" || fail "CAP_MLO_CAP max simultaneous links is not decoded"
grep -q 'if (len < sizeof' <<< "$mc" || fail "capability record is read without a length check"

printf 'policy-modes contract: PASS\n'
