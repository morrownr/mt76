#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
contract=$script_dir/monitor-owner-scope-contract.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mt7927-owner-scope.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp/mt7925"
cat >"$tmp/mt7925/main.c" <<'EOF'
static int
mt7927_reconfig_band(struct mt792x_dev *dev, struct mt792x_bss_conf *mconf,
		     struct mt76_wcid *wcid, struct ieee80211_vif *vif,
		     struct ieee80211_bss_conf *link_conf,
		     const struct cfg80211_chan_def *def)
{
	struct mt7927_reconfig_data data = {};

	data.old_sniffer_active =
		data.old_monitor.state == MT792X_MONITOR_ACTIVE &&
		data.old_monitor.owner == vif &&
		data.old_monitor.link_id == link_conf->link_id;
	return 0;
}

static int mt7925_assign_vif_chanctx(void)
{
	return 0;
}
EOF

"$contract" "$tmp" >/dev/null

sed -i '/data.old_monitor.owner == vif/d' "$tmp/mt7925/main.c"
if "$contract" "$tmp" >"$tmp/out" 2>&1; then
	echo "FAIL: owner-scope mutant unexpectedly passed" >&2
	exit 1
fi
grep -Fqx \
	'FAIL: active sniffer ownership is not scoped to the current VIF and link' \
	"$tmp/out"

echo "PASS: MT7927 monitor owner scope contract selftest"
