#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
contract=$script_dir/void-callback-recovery-contract.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mt7927-void-recovery.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp/mt7925"
cat >"$tmp/mt7925/main.c" <<'EOF'
static void mt7925_chanctx_failure(struct mt792x_dev *dev,
				   const char *operation, int err)
{
	dev_err(dev->mt76.dev, "%s failed: %d\n", operation, err);
	mt792x_reset(&dev->mt76);
}

static void mt7925_sniffer_interface_iter(void)
{
	int err;
	struct mt7925_sniffer_iter_data *data;
	struct mt792x_dev *dev;
	struct ieee80211_vif *vif;

	err = mt7925_mcu_config_sniffer(vif, 0, 0);
	if (err < 0) {
		if (mt7925_mcu_set_sniffer(dev, vif, false, 0) < 0)
			data->reset_required = true;
	}
}

void mt7925_set_runtime_pm(void)
{
}

static void mt7925_change_chanctx(void)
{
	int err = -EIO;
	struct mt792x_dev *dev;

	mt7925_chanctx_failure(dev, "change chanctx", err);
}

static void mt7925_mgd_prepare_tx(void)
{
}

static void mt7925_unassign_vif_chanctx(void)
{
	int err = -EIO;
	struct mt792x_dev *dev;

	mt7925_chanctx_failure(dev, "unassign chanctx", err);
}

static void mt7925_rfkill_poll(void)
{
}

static void mt7925_configure_filter(void)
{
	int err = -EIO;
	struct mt792x_dev *dev;

	mt7925_chanctx_failure(dev, "configure filter", err);
}

static u8 mt7925_get_rates_table(void)
{
	return 0;
}
EOF

"$contract" "$tmp" >/dev/null

cp "$tmp/mt7925/main.c" "$tmp/mt7925/main.c.good"
sed -i '/mt7925_mcu_set_sniffer(dev, vif, false, 0)/d' "$tmp/mt7925/main.c"
if "$contract" "$tmp" >"$tmp/out" 2>&1; then
	echo "FAIL: sniffer rollback mutant unexpectedly passed" >&2
	exit 1
fi
grep -Fqx 'FAIL: sniffer configuration failure has no disable rollback' \
	"$tmp/out"
mv "$tmp/mt7925/main.c.good" "$tmp/mt7925/main.c"

for function in mt7925_change_chanctx mt7925_unassign_vif_chanctx \
	mt7925_configure_filter; do
	cp "$tmp/mt7925/main.c" "$tmp/mt7925/main.c.good"
	sed -i "/static void $function/,/^}/s/mt7925_chanctx_failure(dev,.*);/err = 0;/" \
		"$tmp/mt7925/main.c"
	if "$contract" "$tmp" >"$tmp/out" 2>&1; then
		echo "FAIL: $function recovery mutant unexpectedly passed" >&2
		exit 1
	fi
	mv "$tmp/mt7925/main.c.good" "$tmp/mt7925/main.c"
done

echo "PASS: MT7927 void callback recovery contract selftest"
