#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
contract=$script_dir/chanctx-member-contract.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mt7927-chanctx-member.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp/mt7925"
cat >"$tmp/mt7925/main.c" <<'EOF'
static int mt7927_change_chanctx_link(struct mt792x_dev *dev,
				      struct ieee80211_vif *vif,
				      struct ieee80211_bss_conf *link_conf,
				      struct mt792x_bss_conf *mconf,
				      struct ieee80211_chanctx_conf *ctx,
				      u32 changed)
{
	bool monitor = dev->monitor.state == MT792X_MONITOR_ACTIVE &&
		       dev->monitor.owner == vif &&
		       dev->monitor.link_id == link_conf->link_id;

	if (monitor)
		return mt7927_reconfig_band(dev, mconf, mconf->mt76.wcid, vif,
					    link_conf, &ctx->def, false, NULL);
	mt7925_mcu_set_chctx(vif->phy, &mconf->mt76, link_conf, ctx);
	if (changed & IEEE80211_CHANCTX_CHANGE_PUNCTURING)
		return mt7925_mcu_set_eht_pp(vif->phy, &mconf->mt76, link_conf,
					      ctx);
	return 0;
}

static void mt7927_change_chanctx_iter(void)
{
}
EOF

"$contract" "$tmp" >/dev/null

sed -i '/mt7925_mcu_set_chctx/d' "$tmp/mt7925/main.c"
if "$contract" "$tmp" >"$tmp/out" 2>&1; then
	echo "FAIL: ordinary-member mutant unexpectedly passed" >&2
	exit 1
fi
grep -Fqx 'FAIL: ordinary MT7927 chanctx member bypasses set_chctx' "$tmp/out"

echo "PASS: MT7927 chanctx member contract selftest"
