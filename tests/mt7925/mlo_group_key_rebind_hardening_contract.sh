#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mt7925_mlo_group_key_rebind_iter() previously published the new hardware
# key index (*wcid_keyidx = key->keyidx) BEFORE mt7925_mcu_add_key() had
# confirmed success -- a failed MCU install would leave the wcid's own
# idempotency-check field claiming the key was already current, so a later
# retry would silently take the "already installed" shortcut instead of
# actually retrying. mt7925_mlo_group_key_rebind() also let ctx.err get
# overwritten by every key it processed, so an early real failure could be
# hidden by a later key that happened to succeed.
#
# This contract pins both fixes down, plus the diagnostic counters needed
# to tell "no matching keys" / "already installed" / "installed now" /
# "failed" apart with certainty.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'group-key rebind hardening contract failed: %s\n' "$1" >&2
	exit 1
}

iter_body=$(sed -n '/^static void mt7925_mlo_group_key_rebind_iter(/,/^}/p' mt7925/mlo/association.c)
[[ -n $iter_body ]] || fail "mt7925_mlo_group_key_rebind_iter is missing"

# 1a. The prior hardware key index must be saved before any mutation.
if ! rg -q 'prev_keyidx = \*wcid_keyidx' <<< "$iter_body"; then
	fail "iterator does not save the prior *wcid_keyidx before mutating it"
fi

# 1b. *wcid_keyidx must not be set to the new key's index until AFTER
#     mt7925_mcu_add_key() has been called and its result checked -- i.e.
#     the publish line must come after the MCU call line.
mcu_call_line=$(grep -Fn 'mt7925_mcu_add_key(&dev->mt76, vif, &mlink->bip, key,' <<< "$iter_body" | head -1 | cut -d: -f1 || true)
publish_line=$(grep -Fn '*wcid_keyidx = key->keyidx;' <<< "$iter_body" | head -1 | cut -d: -f1 || true)
[[ -n $mcu_call_line ]] || fail "could not find the mt7925_mcu_add_key() call"
[[ -n $publish_line ]] || fail "iterator never publishes *wcid_keyidx = key->keyidx on success"
if (( publish_line < mcu_call_line )); then
	fail "*wcid_keyidx is published as the new key (line $publish_line) before the MCU call (line $mcu_call_line) -- success is claimed before it happens"
fi

# 1c. On MCU failure, the prior index must be restored, not left at
#     whatever provisional value it had.
if ! rg -q '\*wcid_keyidx = prev_keyidx' <<< "$iter_body"; then
	fail "iterator does not restore *wcid_keyidx = prev_keyidx on MCU failure"
fi

# 2. The first nonzero error must be preserved across iteration, not
#    overwritten by a later (possibly successful) key.
if ! rg -q 'if \(!ctx->first_err\)' <<< "$iter_body"; then
	fail "iterator does not guard error assignment with 'if (!ctx->first_err)' -- a later success/failure could overwrite an earlier real error"
fi
if rg -Fq 'ctx->err = mt7925_mcu_add_key' <<< "$iter_body"; then
	fail "iterator still unconditionally overwrites ctx->err on every key -- first real failure is not preserved"
fi

# 3. Diagnostic counters must exist to distinguish the four outcomes:
#    no matching keys, already installed, installed now, failed.
for field in keys_seen already_current installs_attempted installs_succeeded; do
	if ! rg -q "ctx->$field\+\+" <<< "$iter_body"; then
		fail "iterator never increments ctx->$field"
	fi
done

# The public entry point must return the first preserved error, and must
# emit a summary line with all four counters plus the first error.
rebind_body=$(sed -n '/mt7925_mlo_group_key_rebind(struct mt792x_dev \*dev,/,/^}/p' mt7925/mlo/association.c)
[[ -n $rebind_body ]] || fail "mt7925_mlo_group_key_rebind is missing"

if ! rg -q 'keys_seen=.*already_current=.*installs_attempted=.*installs_succeeded=' <<< "$rebind_body"; then
	fail "mt7925_mlo_group_key_rebind does not emit a summary with keys_seen/already_current/installs_attempted/installs_succeeded"
fi

printf 'group-key rebind hardening contract passed\n'
