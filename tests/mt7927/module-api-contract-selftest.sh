#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
contract=$script_dir/module-api-contract.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mt7927-module-api.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp/good/mt7925" "$tmp/bad/mt7925"

cat >"$tmp/good/mt7925/main.c" <<'EOF'
void caller(struct mt76_phy *phy)
{
	mt76_txq_schedule_pending(phy);
}
EOF
cp "$tmp/good/mt7925/main.c" "$tmp/bad/mt7925/main.c"

cat >"$tmp/good/tx.c" <<'EOF'
void mt76_txq_schedule_pending(struct mt76_phy *phy) {}
EXPORT_SYMBOL_GPL(mt76_txq_schedule_pending);
EOF
cat >"$tmp/bad/tx.c" <<'EOF'
void mt76_txq_schedule_pending(struct mt76_phy *phy) {}
EOF

"$contract" "$tmp/good" >/dev/null
if "$contract" "$tmp/bad" >"$tmp/bad.out" 2>&1; then
	printf 'FAIL: unexported cross-module helper unexpectedly passed\n' >&2
	exit 1
fi
grep -Fq 'called by mt7925-common.ko but is not exported by mt76.ko' \
	"$tmp/bad.out"

printf 'PASS: MT7927 module API contract selftest\n'
