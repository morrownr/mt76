#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
contract=$script_dir/monitor-owner-lifetime-contract.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mt7927-owner-life.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp/mt7925"
cat >"$tmp/mt7925/main.c" <<'EOF'
static void
mt7925_monitor_reset(struct mt792x_monitor_lifecycle *lifecycle)
{
	*lifecycle = (struct mt792x_monitor_lifecycle){};
}

static void
mt7925_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt792x_dev *dev = mt792x_hw_dev(hw);

	if (dev->monitor.owner == vif)
		mt7925_monitor_reset(&dev->monitor);
	mt792x_remove_interface(hw, vif);
}

static bool
mt7925_monitor_owner_candidate(struct mt792x_monitor_lifecycle *lifecycle,
			       struct ieee80211_vif *vif)
{
	if (lifecycle->state == MT792X_MONITOR_OFF)
		return vif->type == NL80211_IFTYPE_MONITOR;

	return lifecycle->owner == vif;
}

static void
mt7925_sniffer_interface_iter(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt792x_monitor_lifecycle *lifecycle = priv;

	if (!mt7925_monitor_owner_candidate(lifecycle, vif))
		return;
}

void mt7925_set_runtime_pm(void)
{
}

static void mt7925_roc_iter(void)
{
}

static const struct ieee80211_ops mt7925_ops = {
	.remove_interface = mt7925_remove_interface,
};
EOF

"$contract" "$tmp" >/dev/null

sed -i 's/mt7925_monitor_reset(&dev->monitor);/dev->monitor.state = MT792X_MONITOR_OFF;/' \
	"$tmp/mt7925/main.c"
if "$contract" "$tmp" >"$tmp/out" 2>&1; then
	echo "FAIL: lifetime mutant unexpectedly passed" >&2
	exit 1
fi
grep -Fqx 'FAIL: interface removal does not retire device-owned monitor state' \
	"$tmp/out"

sed -i 's/dev->monitor.state = MT792X_MONITOR_OFF;/mt7925_monitor_reset(\&dev->monitor);/' \
	"$tmp/mt7925/main.c"
sed -i 's/vif->type == NL80211_IFTYPE_MONITOR/true/' \
	"$tmp/mt7925/main.c"
if "$contract" "$tmp" >"$tmp/out" 2>&1; then
	echo "FAIL: ordinary-VIF owner mutant unexpectedly passed" >&2
	exit 1
fi
grep -Fqx 'FAIL: initial monitor owner is not restricted to a monitor VIF' \
	"$tmp/out"

echo "PASS: MT7927 monitor owner lifetime contract selftest"
