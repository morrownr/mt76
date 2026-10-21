#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Same-token two-TLV JOIN extension contract: the diagnostic gate is
# default off, it never allocates a fresh token (reuses the held one), it
# never sends a survivor TLV alone (reuses the same 2-TLV builder as a
# normal JOIN), and its timeout path never aborts the continuity owner's
# own (token, bss_idx) -- only the replacement's, via the shared guard.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'same-token extend contract failed: %s\n' "$1" >&2
	exit 1
}

main=mt7925/mlo/radio.c
mlo=mt7925/mlo/lifecycle.c
param=mt7925/mlo/lifecycle.c
hdr=mt7925/mlo/decls.h
roc_state=mt7925/mlo_roc_state.h

fn() { sed -n "/^$1(/,/^}/p" "$2"; }

# default off
grep -q '^bool mt7925_mlo_same_token_extend;$' $param ||
	fail "mlo_same_token_extend is not a plain bool (must default false)"
grep -q 'module_param_named(mlo_same_token_extend, mt7925_mlo_same_token_extend' $param ||
	fail "mlo_same_token_extend module param missing"

# eligibility helper exists and is pure (no I/O, no MCU calls)
e=$(fn 'mt7925_mlo_same_token_extend_eligible' $roc_state)
[[ -n $e ]] || fail "eligibility helper missing from mlo_roc_state.h"
grep -q 'mcu_send_msg\|mt76_mcu' <<< "$e" && fail "eligibility helper is not pure"

# the send function reuses the held token, not a fresh one
f=$(fn 'static int mt7925_mlo_roc_same_token_extend' $main)
[[ -n $f ]] || fail "mt7925_mlo_roc_same_token_extend is missing"
grep -q 'token = phy->mlo_roc.held_token;' <<< "$f" ||
	fail "same-token extend does not reuse the held token"
grep -q 'roc_token_id' <<< "$f" &&
	fail "same-token extend must not allocate a fresh token"

# it sends the SAME 2-TLV builder as a normal JOIN, not the 1-TLV attach one
grep -q 'mt7925_mcu_set_mlo_roc(phy, survivor, sel_links, 5, token)' <<< "$f" ||
	fail "same-token extend does not use the 2-TLV mt7925_mcu_set_mlo_roc() builder"
grep -q 'mt7925_mcu_set_mlo_attach_roc' <<< "$f" &&
	fail "same-token extend must not reuse the 1-TLV attach-only builder"

# timeout keeps the owner: only the added link's slot is cancelled, and it
# goes through the shared continuity guard before any abort is sent
grep -q 'mt7925_mlo_roc_state_attach_timeout(&phy->mlo_roc, &tok)' <<< "$f" ||
	fail "timeout path does not use the attach-timeout bookkeeping (would touch held_links)"
grep -q 'mt7925_mlo_roc_guard_links(phy, mvif, tok, cancel' <<< "$f" ||
	fail "timeout path does not route the cancel through the continuity guard"
grep -q 'MLO_SAME_TOKEN_EXTEND_TIMEOUT_KEEP_OWNER' <<< "$f" ||
	fail "timeout path does not log MLO_SAME_TOKEN_EXTEND_TIMEOUT_KEEP_OWNER"

# never abort the survivor's own bss unconditionally
grep -qE 'mt7925_mcu_abort_mlo_roc\(phy, survivor' <<< "$f" &&
	fail "same-token extend must never unconditionally abort the survivor's own bss"

# the delta-acquire call site only takes this path when explicitly gated
d=$(fn 'int mt7925_mlo_roc_delta_acquire' $main)
[[ -n $d ]] || fail "mt7925_mlo_roc_delta_acquire is missing"
grep -q 'READ_ONCE(mt7925_mlo_same_token_extend)' <<< "$d" ||
	fail "delta_acquire does not gate the same-token extend behind the param"
grep -q 'mt7925_mlo_same_token_extend_eligible(st, sel_links)' <<< "$d" ||
	fail "delta_acquire does not check eligibility before extending"

# extern declared for cross-file use
grep -q 'extern bool mt7925_mlo_same_token_extend;' $hdr ||
	fail "mt7925_mlo_same_token_extend extern missing from mlo/decls.h"

echo "PASS: mlo_same_token_extend_contract"
