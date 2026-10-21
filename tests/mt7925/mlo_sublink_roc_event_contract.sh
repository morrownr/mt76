#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mt7925_set_sublink_roc() could never observe success or failure: the
# UNI_EVENT_ROC_GRANT_SUB_LINK (tag=4) handler only logged the event and
# never touched phy->roc_grant/roc_wait, so the function's own
# wait_event_timeout() was guaranteed to time out regardless of what
# firmware did with the request -- proven by source inspection, not
# assumed from tag=4 alone (the task explicitly warned against assuming
# tag=4 implies success; status must be read and honored).
#
# This contract pins down dedicated, token-matched completion state so a
# real grant, an explicit firmware rejection, and a genuine timeout are all
# distinguishable, and so a stale or unrelated event can never satisfy a
# wait it wasn't sent for.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'sublink ROC event contract failed: %s\n' "$1" >&2
	exit 1
}

# 1. Dedicated completion state must exist (never reuse roc_grant/roc_wait's
#    predicate directly for this -- that would let a full-JOIN GRANT event
#    or a stale sublink event satisfy the wrong wait).
if ! rg -q 'u8 sublink_roc_token_id;' mt792x.h; then
	fail "no dedicated sublink_roc_token_id field"
fi
if ! rg -q 'bool sublink_roc_done;' mt792x.h; then
	fail "no dedicated sublink_roc_done completion flag"
fi
if ! rg -q 'u8 sublink_roc_status;' mt792x.h; then
	fail "no dedicated sublink_roc_status field to preserve firmware's status byte"
fi

full_event_fn=$(sed -n '/^mt7925_mcu_uni_roc_event(struct mt792x_dev \*dev, struct sk_buff \*skb)/,/^}/p' mt7925/mcu.c)
[[ -n $full_event_fn ]] || fail "mt7925_mcu_uni_roc_event is missing"

sub_link_case=$(awk '/case UNI_EVENT_ROC_GRANT_SUB_LINK:/{flag=1} flag{print} flag && /^\t\t\}/{exit}' <<< "$full_event_fn")
[[ -n $sub_link_case ]] || fail "could not isolate the UNI_EVENT_ROC_GRANT_SUB_LINK case"

# 2. Tag=4 must be parsed and validated, not merely logged -- a dedicated
#    handler function must exist and be called.
if ! rg -q 'mt7925_mcu_roc_handle_grant_sub_link' <<< "$sub_link_case"; then
	fail "UNI_EVENT_ROC_GRANT_SUB_LINK case does not call a dedicated handler function"
fi

handler_body=$(sed -n '/^static void mt7925_mcu_roc_handle_grant_sub_link(/,/^}/p' mt7925/mcu.c)
[[ -n $handler_body ]] || fail "mt7925_mcu_roc_handle_grant_sub_link is missing"
handler_body+=$'\n'$(sed -n '/^bool mt7927_mlo_radio_event(/,/^}/p' mt7925/mlo/radio.c)

# 3. Event length must be validated before reading fields (do not trust
#    tag=4 alone; do not read past a short/malformed TLV).
if ! rg -q 'len < sizeof\(\*grant\)|len < sizeof\(struct mt7925_roc_grant_tlv\)' <<< "$handler_body"; then
	fail "handler does not validate tlv length against the grant TLV's size before reading fields"
fi

# 4. The event's token must be compared against the outstanding
#    sublink_roc_token_id before completing anything -- and a mismatch (or
#    no outstanding request, token_id==0) must return without setting
#    sublink_roc_done.
if ! rg -q 'tokenid.*sublink_roc_token_id|sublink_roc_token_id.*tokenid' <<< "$handler_body"; then
	fail "handler does not compare the event's tokenid against sublink_roc_token_id"
fi

done_set_line=$(grep -Fn 'sublink_roc_done = true' <<< "$handler_body" | head -1 | cut -d: -f1 || true)
first_return_line=$(grep -Fn 'return;' <<< "$handler_body" | head -1 | cut -d: -f1 || true)
[[ -n $done_set_line ]] || fail "handler never sets sublink_roc_done = true on a real match"
[[ -n $first_return_line ]] || fail "handler has no early return path (needed for a token mismatch / no-outstanding-request case)"
if (( first_return_line > done_set_line )); then
	fail "no early return appears before sublink_roc_done is set (line $done_set_line) -- a mismatched/stale event has no path to bail out without completing the wait"
fi

# 5. Status must be preserved (not ignored) -- stored for the caller to
#    interpret as success/rejection.
if ! rg -q 'sublink_roc_status = ' <<< "$handler_body"; then
	fail "handler does not preserve the firmware status byte into sublink_roc_status"
fi

# 6. The waiter must only be woken for the matching operation -- wake_up
#    must appear strictly after the token-match confirmation and the
#    status/done assignment, not unconditionally at the top.
wake_line=$(grep -Fn 'wake_up' <<< "$handler_body" | head -1 | cut -d: -f1 || true)
[[ -n $wake_line ]] || fail "handler never wakes the waiter on a real match"
if (( wake_line < done_set_line )); then
	fail "wake_up (line $wake_line) happens before sublink_roc_done is set (line $done_set_line)"
fi

# 7. mt7925_set_sublink_roc() itself: state must be initialized before the
#    MCU send and cleared on every exit path (success, MCU send failure,
#    and timeout).
setroc_body=$(sed -n '/^int mt7925_set_sublink_roc(/,/^}/p' mt7925/mlo/radio.c)
[[ -n $setroc_body ]] || fail "mt7925_set_sublink_roc is missing"

if ! rg -q 'sublink_roc_done = false' <<< "$setroc_body"; then
	fail "mt7925_set_sublink_roc does not reset sublink_roc_done before sending"
fi
if ! rg -q 'sublink_roc_token_id = ' <<< "$setroc_body"; then
	fail "mt7925_set_sublink_roc does not record its own token into sublink_roc_token_id before sending"
fi

clear_count=$(grep -Fc 'sublink_roc_token_id = 0' <<< "$setroc_body" || true)
if (( clear_count < 2 )); then
	fail "sublink_roc_token_id is not cleared on at least two distinct exit paths (found $clear_count) -- must be cleared on MCU-send failure, timeout, and completion"
fi

if ! rg -q 'wait_event_timeout\(phy->roc_wait, phy->sublink_roc_done' <<< "$setroc_body"; then
	fail "mt7925_set_sublink_roc does not wait on the dedicated sublink_roc_done predicate"
fi

# 8. Result must reflect firmware status: zero status -> success (0),
#    nonzero status -> an error, not a blind 'received event == success'.
if ! rg -q 'sublink_roc_status' <<< "$setroc_body"; then
	fail "mt7925_set_sublink_roc never inspects sublink_roc_status to distinguish grant from rejection"
fi

printf 'sublink ROC event contract passed\n'
