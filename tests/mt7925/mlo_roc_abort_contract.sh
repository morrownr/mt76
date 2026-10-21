#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

fail()
{
	printf 'MLO ROC abort contract failed: %s\n' "$1" >&2
	exit 1
}

extract_function()
{
	local name=$1
	awk -v name="$name" '
		$0 ~ "^" name "\\(" { found = 1 }
		found {
			print
			line = $0
			opens = gsub(/\{/, "{", line)
			closes = gsub(/\}/, "}", line)
			depth += opens - closes
			if (opens)
				body = 1
			if (body && depth == 0)
				exit
		}
	'
}

set_mlo_roc=$(extract_function "int mt7925_set_mlo_roc" < mt7925/mlo/radio.c)
set_roc=$(extract_function "static int mt7925_set_roc" < mt7925/main.c)
begin_roc=$(extract_function "int mt7927_mlo_radio_begin_offchannel" < mt7925/mlo/radio.c)

# A timed-out JOIN aborts only the slots the request owns exclusively, under
# its own token; slots the held reservation still owns (the survivor's) are
# never aborted under the timed-out token.
if ! rg -q 'mt7925_mlo_roc_state_timeout\(&phy->mlo_roc, &tok\)' <<< "$set_mlo_roc" ||
   ! rg -q 'mt7925_abort_mlo_roc_slots\(phy, mconf->vif, tok, abort_mask\)' <<< "$set_mlo_roc"; then
	fail "mt7925_set_mlo_roc does not abort timed-out request slots under the request token"
fi

if rg -q 'mt7925_abort_mlo_roc_all' <<< "$set_mlo_roc"; then
	fail "mt7925_set_mlo_roc timeout must not release the held reservation"
fi

if rg -q 'mt7925_mcu_abort_roc\(phy, mconf, phy->roc_token_id\)' <<< "$set_mlo_roc"; then
	fail "mt7925_set_mlo_roc still uses generic 0xff abort on its active token"
fi

if ! rg -q 'mt7925_abort_roc_token\(phy, mconf, phy->roc_token_id, false\)' <<< "$set_roc"; then
	fail "mt7925_set_roc timeout does not use the link-aware abort selector"
fi

release_line=$(grep -n 'mt7927_mlo_radio_begin_offchannel(phy, mconf)' <<< "$set_roc" |
	head -1 | cut -d: -f1 || true)
acquire_line=$(grep -n 'mt7925_mcu_set_roc(phy, mconf,' <<< "$set_roc" |
	head -1 | cut -d: -f1 || true)
if [[ -z $release_line || -z $acquire_line || $release_line -ge $acquire_line ]] ||
   ! rg -q 'mt7925_abort_mlo_roc_all\(phy, mconf->vif\)' <<< "$begin_roc"; then
	fail "plain ROC acquire does not first release the stale MLO reservation"
fi

printf 'MLO ROC abort contract passed\n'
