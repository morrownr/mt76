#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Stage 2 only runs after the wiphy-work barrier -- ieee80211_activate_
# links_work()'s entire key_switch_links() loop, covering GTK/IGTK/BIGTK,
# has already returned. So keys_seen==0 at this point is a real "no group
# key for this link" signal, not a race: it is now TERMINAL (consumes
# pending, sends no ROC), replacing the old "retriable, leave pending"
# behavior that assumed an unbarriered, possibly-premature callback.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'key readiness contract failed: %s\n' "$1" >&2
	exit 1
}

# 1. Separate GTK/IGTK/BIGTK counters exist on the shared result/ctx
#    structs.
result_struct=$(awk '/^struct mt7925_group_key_rebind_result \{/{flag=1} flag{print} flag && /^};/{exit}' mt7925/mlo/decls.h)
[[ -n $result_struct ]] || fail "struct mt7925_group_key_rebind_result not found"
for field in gtk_seen igtk_seen bigtk_seen; do
	if ! rg -q "$field" <<< "$result_struct"; then
		fail "mt7925_group_key_rebind_result is missing counter: $field"
	fi
done

ctx_struct=$(awk '/^struct mt7925_group_key_rebind_ctx \{/{flag=1} flag{print} flag && /^};/{exit}' mt7925/mlo/association.c)
[[ -n $ctx_struct ]] || fail "struct mt7925_group_key_rebind_ctx not found"
for field in gtk_seen igtk_seen bigtk_seen; do
	if ! rg -q "$field" <<< "$ctx_struct"; then
		fail "mt7925_group_key_rebind_ctx is missing counter: $field"
	fi
done

iter_body=$(awk '/^static void mt7925_mlo_group_key_rebind_iter\(/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/association.c)
[[ -n $iter_body ]] || fail "mt7925_mlo_group_key_rebind_iter() not found"
for field in 'ctx->gtk_seen++' 'ctx->igtk_seen++' 'ctx->bigtk_seen++'; do
	if ! rg -Fq "$field" <<< "$iter_body"; then
		fail "iterator never increments $field"
	fi
done

# 2. mt7925_mlo_group_key_rebind() propagates all three counters into the
#    result struct.
rebind_body=$(awk '/^int mt7925_mlo_group_key_rebind\(/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/association.c)
[[ -n $rebind_body ]] || fail "mt7925_mlo_group_key_rebind() not found"
for field in 'result->gtk_seen = ctx.gtk_seen' 'result->igtk_seen = ctx.igtk_seen' 'result->bigtk_seen = ctx.bigtk_seen'; do
	if ! rg -Fq "$field" <<< "$rebind_body"; then
		fail "mt7925_mlo_group_key_rebind() does not propagate: $field"
	fi
done

# 3. postkey_attempt_locked(): keys_seen==0 is terminal -- consumes
#    pending via mt7927_mlo_pending_clear(), never reaches the ROC call
#    for that path, and is explicitly traced POSTKEY_KEYS_UNAVAILABLE.
attempt_body=$(awk '/mt7927_mlo_postkey_attempt_locked\(struct mt792x_dev/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $attempt_body ]] || fail "mt7927_mlo_postkey_attempt_locked() not found"

if ! rg -Fq 'POSTKEY_KEYS_UNAVAILABLE' <<< "$attempt_body"; then
	fail "no POSTKEY_KEYS_UNAVAILABLE trace for the keys_seen==0 terminal path"
fi

keys_seen_block=$(awk '/key_result.keys_seen == 0/{flag=1} flag{print} flag && /^\t\}/{exit}' <<< "$attempt_body")
[[ -n $keys_seen_block ]] || fail "could not isolate the keys_seen==0 block"
if ! rg -Fq 'mt7927_mlo_pending_clear(msta)' <<< "$keys_seen_block"; then
	fail "keys_seen==0 block does not consume pending via mt7927_mlo_pending_clear(msta)"
fi
if rg -Fq 'mt7925_set_sublink_roc(' <<< "$keys_seen_block"; then
	fail "keys_seen==0 block sends ROC -- must be terminal, no ROC"
fi

# 4. Exactly one standalone ROC send site.
roc_send_count=$(grep -Fc 'mt7925_set_sublink_roc(phy, sub_mconf,' <<< "$attempt_body" || true)
if [[ $roc_send_count -ne 1 ]]; then
	fail "expected exactly one mt7925_set_sublink_roc() call site, found $roc_send_count"
fi

printf 'key readiness contract passed\n'
