#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# mt7925_mac_sta_add_links() unconditionally publishes msta->valid_links
# and calls mt7925_mac_link_sta_assoc(), discarding
# mt7925_mlo_reconcile_link()'s error internally -- reusing it wholesale
# for the postkey grant path let a real grant + hidden reconcile failure
# leave the substitute valid_links-published with broken keys. This
# contract pins the prepare/commit split that closes that gap.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'prepare/commit contract failed: %s\n' "$1" >&2
	exit 1
}

prepare_body=$(awk '/^mt7927_mlo_postkey_prepare\(/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $prepare_body ]] || fail "mt7927_mlo_postkey_prepare() is missing"

# Prepare creates/reuses the peer link object and reconciles keys with
# errors propagated (a real 'return err;'/'return -E...;' path, not
# discarded).
if ! rg -Fq 'mt7925_mac_sta_add_links(' <<< "$prepare_body"; then
	fail "prepare does not create the peer link object via mt7925_mac_sta_add_links()"
fi
if ! rg -Fq 'mt7925_mlo_reconcile_link(' <<< "$prepare_body"; then
	fail "prepare does not reconcile keys via mt7925_mlo_reconcile_link()"
fi
reconcile_line=$(grep -Fn 'err = mt7925_mlo_reconcile_link(' <<< "$prepare_body" | head -1 | cut -d: -f1 || true)
[[ -n $reconcile_line ]] || fail "prepare does not capture mt7925_mlo_reconcile_link()'s return value into err"

# Prepare must NOT set valid_links, wcid.link_valid, associate, or touch
# traffic_active_links -- those are commit-only.
for forbidden in 'valid_links |=' 'wcid.link_valid = true' 'mt7925_mac_link_sta_assoc(' 'traffic_active_links |='; do
	if rg -Fq "$forbidden" <<< "$prepare_body"; then
		fail "prepare performs a commit-only action: $forbidden"
	fi
done

attempt_body=$(awk '/mt7927_mlo_postkey_attempt_locked\(struct mt792x_dev/{flag=1} flag{print} flag && /^}/{exit}' mt7925/mlo/lifecycle.c)
[[ -n $attempt_body ]] || fail "mt7927_mlo_postkey_attempt_locked() not found"

if ! rg -Fq 'mt7927_mlo_postkey_prepare(' <<< "$attempt_body"; then
	fail "postkey_attempt_locked() does not call mt7927_mlo_postkey_prepare()"
fi
prepare_call_line=$(grep -Fn 'mt7927_mlo_postkey_prepare(' <<< "$attempt_body" | head -1 | cut -d: -f1)

# Commit actions (associate once, stamp admission/generation, publish
# valid_links, set wcid valid, add traffic_active_links) must all appear
# textually AFTER the prepare call, gated by its success.
for committed in 'mt7925_mac_link_sta_assoc(' 'roc_admitted = true' 'admitted_generation =' 'valid_links |=' 'wcid.link_valid = true' 'traffic_active_links |='; do
	commit_line=$(grep -Fn "$committed" <<< "$attempt_body" | head -1 | cut -d: -f1 || true)
	[[ -n $commit_line ]] || fail "postkey_attempt_locked() never commits: $committed"
	if (( commit_line < prepare_call_line )); then
		fail "'$committed' is committed (line $commit_line) before prepare is called (line $prepare_call_line)"
	fi
done

# Associate exactly once for the grant path -- and skip it entirely when
# prepare itself created the peer, since mt7925_mac_sta_add_links() (called
# from inside prepare for that case) already sends its own association
# command internally; calling it again would double-associate the link.
assoc_count=$(grep -Fc 'mt7925_mac_link_sta_assoc(' <<< "$attempt_body" || true)
if [[ $assoc_count -ne 1 ]]; then
	fail "expected exactly one mt7925_mac_link_sta_assoc() call site in postkey_attempt_locked(), found $assoc_count"
fi
if ! rg -Fq ', &created);' <<< "$attempt_body"; then
	fail "postkey_attempt_locked() does not pass an out_created flag to mt7927_mlo_postkey_prepare()"
fi
if ! rg -Fq 'if (!created)' <<< "$attempt_body"; then
	fail "postkey_attempt_locked() does not skip mt7925_mac_link_sta_assoc() when prepare already created (and thus already associated) the peer"
fi

printf 'prepare/commit contract passed\n'
