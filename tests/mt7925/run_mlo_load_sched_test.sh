#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bin=$(mktemp /tmp/mt7925-mlo-load-sched.XXXXXX)
trap 'rm -f "$bin"' EXIT

cc -std=c11 -Wall -Wextra -Werror -I"$root" \
	"$root/tests/mt7925/mlo_load_sched_test.c" -o "$bin"
"$bin"
