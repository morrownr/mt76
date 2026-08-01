#!/bin/sh

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}
script="$root/codex/mt7927-mlo-validate.sh"

fail()
{
	printf 'FAIL: %s\n' "$1" >&2
	exit 1
}

[ -f "$script" ] || fail "validation helper is missing"

# shellcheck disable=SC2016
grep -Fq 'SAE_PASSWORD="${SAE_PASSWORD:-}"' "$script" ||
	fail "SAE password must come from the environment"

grep -Fq 'SAE_PASSWORD is required' "$script" ||
	fail "validation helper must reject a missing SAE password"

if grep -Eq '^SAE_PASSWORD="\$\{SAE_PASSWORD:-[^}]+' "$script"; then
	fail "validation helper contains a default SAE credential"
fi

printf 'PASS: MLO validation helper does not embed an SAE credential\n'
