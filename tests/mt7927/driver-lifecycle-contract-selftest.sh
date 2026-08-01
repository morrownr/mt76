#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
contract=$script_dir/driver-lifecycle-contract.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mt7927-contract.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

selftest_failures=0

fail()
{
	printf 'FAIL: %s\n' "$1" >&2
	selftest_failures=$((selftest_failures + 1))
}

validate_c()
{
	fixture=$1
	for source in mt7925/mcu.c mt7925/main.c mt76_connac_mcu.c; do
		if ! cc -std=c11 -fsyntax-only -I"$fixture" \
			"$fixture/$source" >"$tmp/cc.out" 2>&1; then
			cat "$tmp/cc.out" >&2
			fail "$fixture/$source is not valid C"
			return 1
		fi
	done
	if ! (cd "$fixture" && printf '#include "mt7925/mt7925.h"\n' |
		cc -std=c11 -fsyntax-only -I. -x c -) \
		>"$tmp/cc.out" 2>&1; then
		cat "$tmp/cc.out" >&2
		fail "$fixture/mt7925/mt7925.h is not valid C"
		return 1
	fi
}

expect_pass()
{
	name=$1
	root=$2
	validate_c "$root" || return
	if ! "$contract" "$root" >"$tmp/$name.out" 2>&1; then
		cat "$tmp/$name.out" >&2
		fail "$name fixture unexpectedly failed"
		return
	fi
	if [ "$(cat "$tmp/$name.out")" != \
		'PASS: MT7927 driver lifecycle transaction contract' ]; then
		cat "$tmp/$name.out" >&2
		fail "$name fixture produced unexpected success output"
	fi
}

expect_exact_diagnostics()
{
	name=$1
	root=$2
	shift 2
	validate_c "$root" || return
	if "$contract" "$root" >"$tmp/$name.out" 2>&1; then
		fail "$name fixture unexpectedly passed"
		return
	fi
	: >"$tmp/$name.expected"
	for diagnostic in "$@"; do
		printf '%s\n' "$diagnostic" >>"$tmp/$name.expected"
	done
	sed -n \
		'/^FAIL: MT7927 driver lifecycle contract ([0-9][0-9]* violations)$/!s/^FAIL: //p' \
		"$tmp/$name.out" | sort >"$tmp/$name.actual"
	sort "$tmp/$name.expected" >"$tmp/$name.expected.sorted"
	if ! cmp -s "$tmp/$name.actual" "$tmp/$name.expected.sorted"; then
		printf '%s\n' "--- $name expected diagnostics" >&2
		cat "$tmp/$name.expected.sorted" >&2
		printf '%s\n' "--- $name actual output" >&2
		cat "$tmp/$name.out" >&2
		fail "$name fixture diagnostic set differs"
	fi
	count=$#
	if ! grep -Fqx \
		"FAIL: MT7927 driver lifecycle contract ($count violations)" \
		"$tmp/$name.out"; then
		cat "$tmp/$name.out" >&2
		fail "$name fixture aggregate count differs"
	fi
}

copy_fixture()
{
	name=$1
	cp -R "$tmp/good" "$tmp/$name"
}

replace_marked_once()
{
	file=$1
	marker=$2
	search=$3
	replacement=$4
	output=$file.mutated
	if ! MARKER=$marker SEARCH=$search REPLACEMENT=$replacement perl -0 -e '
		my ($source, $result) = (do { local $/; <> }, q{});
		my ($marked, $replaced) = (0, 0);
		for my $line (split /(?<=\n)/, $source) {
			if (index($line, $ENV{MARKER}) >= 0) {
				$marked++;
				my $count = () = $line =~ /\Q$ENV{SEARCH}\E/g;
				die "marked replacement count is $count, expected 1\n"
					unless $count == 1;
				$line =~ s/\Q$ENV{SEARCH}\E/$ENV{REPLACEMENT}/;
				$replaced += $count;
			}
			$result .= $line;
		}
		die "marked line count is $marked, expected 1\n"
			unless $marked == 1 && $replaced == 1;
		print $result;
	' "$file" >"$output"; then
		rm -f "$output"
		fail "$file did not contain exactly one $marker replacement"
		return
	fi
	mv "$output" "$file"
}

swap_adjacent_marked_lines()
{
	file=$1
	first=$2
	second=$3
	FIRST=$first SECOND=$second perl -0pi -e '
		my $first = quotemeta $ENV{FIRST};
		my $second = quotemeta $ENV{SECOND};
		s/^([^\n]*$first[^\n]*\n)([^\n]*$second[^\n]*\n)/$2$1/m
	' "$file"
}

swap_adjacent_marked_blocks_once()
{
	file=$1
	first=$2
	second=$3
	output=$file.mutated
	if ! FIRST=$first SECOND=$second perl -0 -e '
		my $source = do { local $/; <> };
		my @lines = split /(?<=\n)/, $source;
		my @first = grep { index($lines[$_], $ENV{FIRST}) >= 0 } 0 .. $#lines;
		my @second = grep { index($lines[$_], $ENV{SECOND}) >= 0 } 0 .. $#lines;
		die "marked block counts are not exactly one each\n"
			unless @first == 1 && @second == 1;
		my ($first_start, $second_start) = ($first[0], $second[0]);
		$first_start-- while $first_start >= 0 &&
			$lines[$first_start] !~ /^\tif\s*\(.*\)\s*\{/;
		$second_start-- while $second_start >= 0 &&
			$lines[$second_start] !~ /^\tif\s*\(.*\)\s*\{/;
		die "marked block start not found\n"
			if $first_start < 0 || $second_start < 0;
		my ($first_end, $second_end) = ($first[0], $second[0]);
		$first_end++ while $first_end <= $#lines &&
			$lines[$first_end] !~ /^\t\}\s*$/;
		$second_end++ while $second_end <= $#lines &&
			$lines[$second_end] !~ /^\t\}\s*$/;
		die "marked block end not found\n"
			if $first_end > $#lines || $second_end > $#lines;
		die "marked blocks are not adjacent and ordered\n"
			unless $first_end + 1 == $second_start;
		my @first_block = @lines[$first_start .. $first_end];
		my @second_block = @lines[$second_start .. $second_end];
		splice @lines, $first_start, $second_end - $first_start + 1,
			@second_block, @first_block;
		print @lines;
	' "$file" >"$output"; then
		rm -f "$output"
		fail "$file did not contain exactly one adjacent $first/$second block swap"
		return
	fi
	mv "$output" "$file"
}

make_fixture()
{
	root=$1
	mkdir -p "$root/mt7925"

	cat >"$root/mt792x.h" <<'EOF'
#ifndef SELFTEST_MT792X_H
#define SELFTEST_MT792X_H
#include <stdbool.h>
typedef unsigned char u8;
struct ieee80211_vif { int marker; };
struct ieee80211_bss_conf { int link_id; };
struct ieee80211_chanctx_conf { int marker; };
struct mt76_worker { int marker; };
struct mt76_phy { int marker; };
struct mt76_dev {
	struct mt76_worker tx_worker;
	int tx_wait;
};
struct mt76_wcid { int phy_idx; };
struct mt76_vif {
	int band_idx;
	void *ctx;
	struct mt76_wcid *wcid;
};
struct mt792x_phy {
	struct ieee80211_vif *monitor_vif;
	void *chandef;
};
struct mt792x_bss_conf {
	struct mt76_vif mt76;
	struct ieee80211_vif *vif;
};
struct mt792x_monitor_lifecycle {
	struct mt792x_phy *active_phy;
	u8 programmed_engine;
};
struct mt792x_dev {
	struct mt76_dev mt76;
	struct mt76_phy mphy;
	struct mt792x_monitor_lifecycle sniffer_lifecycle;
	int mutex;
};
struct mt792x_vif {
	int basic_rates_idx;
};
#endif
EOF

	cat >"$root/fixture.h" <<'EOF'
#ifndef SELFTEST_FIXTURE_H
#define SELFTEST_FIXTURE_H
#include "mt792x.h"
#define BIT(n) (1UL << (n))
#define EINVAL 22
#define ENOENT 2
enum {
	NL80211_BAND_2GHZ,
	NL80211_BAND_5GHZ,
	NL80211_BAND_6GHZ,
	CHANCTX_SWMODE_REASSIGN_VIF,
	CHANCTX_SWMODE_SWAP_CONTEXTS,
};
struct ieee80211_vif_chanctx_switch {
	struct mt792x_bss_conf *mconf;
	int band_idx;
	void *new_ctx;
};
extern int reset_work;
void mt76_worker_disable(struct mt76_worker *worker);
void mt76_worker_enable(struct mt76_worker *worker);
int mt76_has_tx_pending(struct mt76_phy *phy);
long wait_event_timeout(int wait, int condition, unsigned long timeout);
int queue_work(int *work);
void mutex_lock(int *lock);
void mutex_unlock(int *lock);
struct mt792x_bss_conf *
mt792x_vif_to_link_exact(struct mt792x_vif *mvif, int link_id);
struct mt792x_bss_conf *
mt792x_vif_to_link(struct mt792x_vif *mvif, int link_id);
int mt7925_mcu_set_sniffer(struct mt792x_dev *dev,
			   struct ieee80211_vif *vif, bool enable,
			   u8 engine);
int mt7925_mcu_config_sniffer(struct ieee80211_vif *vif,
			      struct ieee80211_chanctx_conf *ctx,
			      u8 engine);
int mt76_connac_mcu_uni_add_dev_info(struct mt792x_bss_conf *mconf,
				     struct mt76_wcid *wcid,
				     bool enable);
int mt76_connac_mcu_uni_add_bss_info(struct mt792x_bss_conf *mconf,
				     struct mt76_wcid *wcid,
				     bool enable);
#endif
EOF

	cat >"$root/mt7925/mt7925.h" <<'EOF'
#ifndef SELFTEST_MT7925_H
#define SELFTEST_MT7925_H
#include "mt792x.h"
int
mt7925_mcu_set_sniffer(
	struct mt792x_dev *dev,
	struct ieee80211_vif *vif,
	bool enable,
	u8 engine
);
int
mt7925_mcu_config_sniffer(
	struct ieee80211_vif *vif,
	struct ieee80211_chanctx_conf *ctx,
	u8 engine
);
#endif
EOF

	cat >"$root/mt7925/mcu.c" <<'EOF'
#include "fixture.h"
int
mt7925_mcu_set_sniffer(
	struct mt792x_dev *dev,
	struct ieee80211_vif *vif,
	bool enable,
	u8 engine
)
{
	struct { struct { u8 band_idx; } hdr; } req = { 0 };
	(void)dev;
	(void)vif;
	(void)enable;
	req.hdr.band_idx
		=
		engine;
	return req.hdr.band_idx;
}

int
mt7925_mcu_config_sniffer(
	struct ieee80211_vif *vif,
	struct ieee80211_chanctx_conf *ctx,
	u8 engine
)
{
	struct { struct { u8 band_idx; } hdr; } req = { 0 };
	const char *escaped = "literal \\\" // still literal";
	(void)escaped;
	(void)vif;
	(void)ctx;
	req.hdr.band_idx = engine;
	return req.hdr.band_idx;
}
EOF

	cat >"$root/mt76_connac_mcu.c" <<'EOF'
#include "fixture.h"
int mt76_connac_mcu_uni_add_dev_info(struct mt792x_bss_conf *mconf,
				     struct mt76_wcid *wcid,
				     bool enable)
{
	return mconf != 0 && wcid != 0 && enable;
}

int mt76_connac_mcu_uni_add_bss_info(struct mt792x_bss_conf *mconf,
				     struct mt76_wcid *wcid,
				     bool enable)
{
	return mconf != 0 && wcid != 0 && enable;
}

int mt76_connac_mcu_uni_add_dev(struct mt792x_bss_conf *mconf,
				struct mt76_wcid *wcid, bool enable)
{
	int err;

	if (enable) { /* legacy:enable-branch */
		err = mt76_connac_mcu_uni_add_dev_info(mconf, wcid, true); /* legacy:enable-dev */
		if (err < 0) /* legacy:enable-check */
			return err;
		return mt76_connac_mcu_uni_add_bss_info(mconf, wcid, true); /* legacy:enable-bss */
	}

	err = mt76_connac_mcu_uni_add_bss_info(mconf, wcid, false); /* legacy:disable-bss */
	if (err < 0) /* legacy:disable-check */
		return err;
	return mt76_connac_mcu_uni_add_dev_info(mconf, wcid, false); /* legacy:disable-dev */
}
EOF

	cat >"$root/mt7925/main.c" <<'EOF'
#include "fixture.h"
enum mt7927_journal_stage {
	MT7927_JOURNAL_OLD_SNIFFER_OFF,
	MT7927_JOURNAL_OLD_BSS_OFF,
	MT7927_JOURNAL_OLD_DEV_OFF,
	MT7927_JOURNAL_NEW_DEV_ON,
	MT7927_JOURNAL_NEW_BSS_ON,
	MT7927_JOURNAL_NEW_SNIFFER_ON,
	MT7927_JOURNAL_NEW_SNIFFER_CONFIG,
	MT7927_JOURNAL_HOST_PUBLISHED,
};

static int mt7925_monitor_sync(struct mt792x_dev *dev)
{
	struct mt792x_phy *phy = dev->sniffer_lifecycle.active_phy;
	u8 engine = dev->sniffer_lifecycle.programmed_engine;
	int err;

	if (!phy)
		return 0;
	err = mt7925_mcu_set_sniffer(dev, phy->monitor_vif, true, engine);
	if (err < 0)
		return err;
	return mt7925_mcu_config_sniffer(phy->monitor_vif, 0, engine);
}

int mt7925_monitor_update_chan(struct mt792x_dev *dev,
			       struct mt792x_phy *phy, u8 engine)
{
	dev->sniffer_lifecycle.active_phy = phy;
	dev->sniffer_lifecycle.programmed_engine = engine;
	return mt7925_monitor_sync(dev);
}

static int mt7927_band_to_engine(int band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return 0;
	case NL80211_BAND_5GHZ:
	case NL80211_BAND_6GHZ:
		return 1;
	default:
		return -EINVAL;
	}
}

static bool mt7927_same_engine(int old_band, int new_band)
{
	return mt7927_band_to_engine(old_band) ==
	       mt7927_band_to_engine(new_band);
}

static int mt7927_old_sniffer_off(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)wcid;
	return mt7925_mcu_set_sniffer(dev, mconf->vif, false,
		mt7927_band_to_engine(mconf->mt76.band_idx));
}
static int mt7927_old_bss_off(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	return mt76_connac_mcu_uni_add_bss_info(mconf, wcid, false);
}
static int mt7927_old_dev_off(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	return mt76_connac_mcu_uni_add_dev_info(mconf, wcid, false);
}
static int mt7927_new_dev_on(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	return mt76_connac_mcu_uni_add_dev_info(mconf, wcid, true);
}
static int mt7927_new_bss_on(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	return mt76_connac_mcu_uni_add_bss_info(mconf, wcid, true);
}
static int mt7927_new_sniffer_on(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)wcid;
	return mt7925_mcu_set_sniffer(dev, mconf->vif, true, 1);
}
static int mt7927_new_sniffer_config(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	(void)wcid;
	return mt7925_mcu_config_sniffer(mconf->vif, mconf->mt76.ctx, 1);
}
static int mt7927_publish_band(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	(void)wcid;
	mconf->mt76.band_idx = NL80211_BAND_5GHZ;
	return 0;
}
static int mt7927_restore_band(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	(void)wcid;
	mconf->mt76.band_idx = NL80211_BAND_2GHZ;
	return 0;
}
static int mt7927_new_sniffer_unconfig(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	(void)mconf;
	(void)wcid;
	return 0;
}
static int mt7927_new_sniffer_off(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)wcid;
	return mt7925_mcu_set_sniffer(dev, mconf->vif, false, 1);
}
static int mt7927_new_bss_off(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	return mt76_connac_mcu_uni_add_bss_info(mconf, wcid, false);
}
static int mt7927_new_dev_off(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	return mt76_connac_mcu_uni_add_dev_info(mconf, wcid, false);
}
static int mt7927_old_dev_on(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	return mt76_connac_mcu_uni_add_dev_info(mconf, wcid, true);
}
static int mt7927_old_bss_on(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)dev;
	return mt76_connac_mcu_uni_add_bss_info(mconf, wcid, true);
}
static int mt7927_old_sniffer_restore(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid)
{
	(void)wcid;
	return mt7925_mcu_set_sniffer(dev, mconf->vif, true,
		mt7927_band_to_engine(mconf->mt76.band_idx));
}

static int mt7927_same_engine_rearm(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid,
		int band_idx)
{
	int err;
	u8 engine = (u8)mt7927_band_to_engine(band_idx);

	(void)wcid;
	err = mt7925_mcu_set_sniffer(dev, mconf->vif, true, engine);
	if (err < 0)
		return err;
	return mt7925_mcu_config_sniffer(mconf->vif, mconf->mt76.ctx,
					   engine);
}

static int mt7927_reconfig_rollback(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid,
		unsigned long journal)
{
	int err;

	if (journal & BIT(MT7927_JOURNAL_HOST_PUBLISHED)) {
		err = mt7927_restore_band(dev, mconf, wcid);
		if (err < 0) return err; /* rollback:restore_band */
	}
	if (journal & BIT(MT7927_JOURNAL_NEW_SNIFFER_CONFIG)) {
		err = mt7927_new_sniffer_unconfig(dev, mconf, wcid);
		if (err < 0) return err; /* rollback:new_sniffer_unconfig */
	}
	if (journal & BIT(MT7927_JOURNAL_NEW_SNIFFER_ON)) {
		err = mt7927_new_sniffer_off(dev, mconf, wcid);
		if (err < 0) return err; /* rollback:new_sniffer_off */
	}
	if (journal & BIT(MT7927_JOURNAL_NEW_BSS_ON)) { /* journal:guard-new_bss_off */
		err = mt7927_new_bss_off(dev, mconf, wcid);
		if (err < 0) return err; /* rollback:new_bss_off */
	}
	if (journal & BIT(MT7927_JOURNAL_NEW_DEV_ON)) {
		err = mt7927_new_dev_off(dev, mconf, wcid);
		if (err < 0) return err; /* rollback:new_dev_off */
	}
	if (journal & BIT(MT7927_JOURNAL_OLD_DEV_OFF)) {
		err = mt7927_old_dev_on(dev, mconf, wcid);
		if (err < 0) return err; /* rollback:old_dev_on */
	}
	if (journal & BIT(MT7927_JOURNAL_OLD_BSS_OFF)) {
		err = mt7927_old_bss_on(dev, mconf, wcid);
		if (err < 0) return err; /* rollback:old_bss_on */
	}
	if (journal & BIT(MT7927_JOURNAL_OLD_SNIFFER_OFF)) {
		err = mt7927_old_sniffer_restore(dev, mconf, wcid);
		if (err < 0) return err; /* rollback:old_sniffer_restore */
	}
	return 0;
}

static int mt7927_reconfig_band(struct mt792x_dev *dev,
		struct mt792x_bss_conf *mconf, struct mt76_wcid *wcid,
		int band_idx)
{
	unsigned long journal = 0; /* journal:init */
	unsigned long timeout = 1;
	int err, primary_err, rollback_err;

	if (mt7927_same_engine(mconf->mt76.band_idx, band_idx))
		return mt7927_same_engine_rearm(dev, mconf, wcid, band_idx);

	mt76_worker_disable(&dev->mt76.tx_worker); /* tx:disable */
	wait_event_timeout(dev->mt76.tx_wait, /* tx:drain-wait */
		!mt76_has_tx_pending(&dev->mphy), /* tx:drain-phy */
		timeout); /* tx:drain */
	err = mt7927_old_sniffer_off(dev, mconf, wcid); if (err < 0) goto rollback; journal |= BIT(MT7927_JOURNAL_OLD_SNIFFER_OFF); /* forward:old_sniffer_off */
	err = mt7927_old_bss_off(dev, mconf, wcid); if (err < 0) goto rollback; journal |= BIT(MT7927_JOURNAL_OLD_BSS_OFF); /* forward:old_bss_off */
	err = mt7927_old_dev_off(dev, mconf, wcid); if (err < 0) goto rollback; journal |= BIT(MT7927_JOURNAL_OLD_DEV_OFF); /* forward:old_dev_off */
	err = mt7927_new_dev_on(dev, mconf, wcid); if (err < 0) goto rollback; journal |= BIT(MT7927_JOURNAL_NEW_DEV_ON); /* forward:new_dev_on */
	err = mt7927_new_bss_on(dev, mconf, wcid); if (err < 0) goto rollback; journal |= BIT(MT7927_JOURNAL_NEW_BSS_ON); /* forward:new_bss_on */
	err = mt7927_new_sniffer_on(dev, mconf, wcid); if (err < 0) goto rollback; journal |= BIT(MT7927_JOURNAL_NEW_SNIFFER_ON); /* forward:new_sniffer_on */
	err = mt7927_new_sniffer_config(dev, mconf, wcid); if (err < 0) goto rollback; journal |= BIT(MT7927_JOURNAL_NEW_SNIFFER_CONFIG); /* forward:new_sniffer_config */
	err = mt7927_publish_band(dev, mconf, wcid); if (err < 0) goto rollback; journal |= BIT(MT7927_JOURNAL_HOST_PUBLISHED); /* forward:publish_band */
	mt76_worker_enable(&dev->mt76.tx_worker); /* tx:forward-resume */
	return 0;

rollback:
	primary_err = err; /* rollback:primary-save */
	rollback_err = mt7927_reconfig_rollback(dev, mconf, wcid, journal); /* journal:rollback-call */
	if (rollback_err < 0) { /* rollback:failure-branch */
		queue_work(&reset_work); /* rollback:failure-reset */
		return primary_err; /* rollback:failure-return */
	}
	mt76_worker_enable(&dev->mt76.tx_worker); /* rollback:success-resume */
	return primary_err; /* rollback:success-return */
}

static int mt7925_assign_vif_chanctx(struct mt792x_dev *dev,
		struct mt792x_vif *mvif,
		struct ieee80211_bss_conf *link_conf,
		struct ieee80211_chanctx_conf *ctx)
{
	struct mt792x_bss_conf *mconf;
	struct mt76_wcid *wcid;
	int err;

	mconf = mt792x_vif_to_link_exact(mvif, link_conf->link_id);
	if (!mconf)
		return -ENOENT;
	wcid = mconf->mt76.wcid; /* mlo:assignment-wcid */
	err = mt7927_reconfig_band(dev, mconf, wcid, NL80211_BAND_5GHZ);
	if (err < 0)
		return err;
	mconf->mt76.ctx = ctx;
	return 0;
}

static void mt7925_unassign_vif_chanctx(struct mt792x_dev *dev,
		struct mt792x_vif *mvif,
		struct ieee80211_bss_conf *link_conf)
{
	struct mt792x_bss_conf *mconf;
	struct mt76_wcid *wcid;

	mconf = mt792x_vif_to_link_exact(mvif, link_conf->link_id);
	if (!mconf)
		return;
	wcid = mconf->mt76.wcid; /* mlo:unassignment-wcid */
	(void)mt7927_reconfig_band(dev, mconf, wcid, NL80211_BAND_2GHZ);
}

static int mt7925_switch_vif_chanctx(struct mt792x_dev *dev,
		struct ieee80211_vif_chanctx_switch *vifs,
		int n_vifs, int mode)
{
	int i, err = 0;

	if (!vifs || n_vifs <= 0)
		return -EINVAL;
	switch (mode) {
	case CHANCTX_SWMODE_REASSIGN_VIF:
	case CHANCTX_SWMODE_SWAP_CONTEXTS:
		break;
	default:
		return -EINVAL;
	}
	mutex_lock(&dev->mutex);
	for (i = 0; i < n_vifs; i++) {
		err = mt7927_reconfig_band(dev, vifs[i].mconf,
			vifs[i].mconf->mt76.wcid, vifs[i].band_idx);
		if (err < 0)
			goto out;
	}
	for (i = 0; i < n_vifs; i++)
		vifs[i].mconf->mt76.ctx = vifs[i].new_ctx;
out:
	mutex_unlock(&dev->mutex);
	return err;
}
EOF
}

[ -x "$contract" ] || fail "contract is not executable"
make_fixture "$tmp/good"
expect_pass good "$tmp/good"

# C translation phases and multiline extraction.
copy_fixture lex-line-splice-comment
perl -0pi -e 's/(mt7925_mcu_set_sniffer\([^;]*?bool enable),\n\s*u8 engine\n/$1\n/s' \
	"$tmp/lex-line-splice-comment/mt7925/mt7925.h"
cat >>"$tmp/lex-line-splice-comment/mt7925/mt7925.h" <<'EOF'
// \
int mt7925_mcu_set_sniffer(struct mt792x_dev *, struct ieee80211_vif *, bool, u8 engine);
EOF
expect_exact_diagnostics lex-line-splice-comment \
	"$tmp/lex-line-splice-comment" \
	"sniffer enable declaration does not take an explicit firmware engine"

copy_fixture lex-block-comment
sed -i '0,/req\.hdr\.band_idx = engine/s//req.hdr.band_idx = 0/' \
	"$tmp/lex-block-comment/mt7925/mcu.c"
sed -i '/req.hdr.band_idx = 0/a\	/* req.hdr.band_idx = engine; */' \
	"$tmp/lex-block-comment/mt7925/mcu.c"
expect_exact_diagnostics lex-block-comment "$tmp/lex-block-comment" \
	"sniffer configuration command does not use its explicit firmware engine"

copy_fixture lex-string
sed -i '0,/req\.hdr\.band_idx = engine/s//req.hdr.band_idx = 0/' \
	"$tmp/lex-string/mt7925/mcu.c"
sed -i '/req.hdr.band_idx = 0/a\	(void)"req.hdr.band_idx = engine;";' \
	"$tmp/lex-string/mt7925/mcu.c"
expect_exact_diagnostics lex-string "$tmp/lex-string" \
	"sniffer configuration command does not use its explicit firmware engine"

copy_fixture lex-preprocessor
sed -i 's/struct mt792x_phy \*active_phy;/void *active_phy;/' \
	"$tmp/lex-preprocessor/mt792x.h"
sed -i '/struct mt792x_monitor_lifecycle {/a\#define FAKE_MONITOR_OWNER struct mt792x_phy *fake_owner;' \
	"$tmp/lex-preprocessor/mt792x.h"
expect_exact_diagnostics lex-preprocessor "$tmp/lex-preprocessor" \
	"monitor lifecycle state has no device-owned PHY and engine"

# Behavior and ownership, independent of exact state/member names.
copy_fixture monitor-bss-owner
sed -i 's/struct mt792x_phy \*active_phy;/struct mt792x_bss_conf *active_phy;/' \
	"$tmp/monitor-bss-owner/mt792x.h"
sed -i 's/struct mt792x_phy \*phy = dev->sniffer_lifecycle.active_phy;/struct mt792x_phy *phy = 0;/' \
	"$tmp/monitor-bss-owner/mt7925/main.c"
sed -i 's/dev->sniffer_lifecycle.active_phy = phy;/dev->sniffer_lifecycle.active_phy = 0;/' \
	"$tmp/monitor-bss-owner/mt7925/main.c"
expect_exact_diagnostics monitor-bss-owner "$tmp/monitor-bss-owner" \
	"monitor lifecycle state has no device-owned PHY and engine"

copy_fixture monitor-shared-bss-authority
sed -i '/struct mt792x_monitor_lifecycle sniffer_lifecycle;/a\	struct mt792x_bss_conf *monitor_conf;' \
	"$tmp/monitor-shared-bss-authority/mt792x.h"
sed -i '/struct mt792x_phy \*phy = dev->sniffer_lifecycle.active_phy;/a\	struct mt792x_bss_conf *shared = dev->monitor_conf;\
\t(void)shared;' "$tmp/monitor-shared-bss-authority/mt7925/main.c"
expect_exact_diagnostics monitor-shared-bss-authority \
	"$tmp/monitor-shared-bss-authority" \
	"monitor lifecycle state makes shared BSS context authoritative"

copy_fixture monitor-sync-owner
sed -i 's/struct mt792x_phy \*phy = dev->sniffer_lifecycle.active_phy;/struct mt792x_phy *phy = 0;/' \
	"$tmp/monitor-sync-owner/mt7925/main.c"
expect_exact_diagnostics monitor-sync-owner "$tmp/monitor-sync-owner" \
	"monitor reconciliation does not consume device-owned PHY and engine"

copy_fixture monitor-sync-engine
sed -i 's/u8 engine = dev->sniffer_lifecycle.programmed_engine;/u8 engine = 0;/' \
	"$tmp/monitor-sync-engine/mt7925/main.c"
expect_exact_diagnostics monitor-sync-engine "$tmp/monitor-sync-engine" \
	"monitor reconciliation does not consume device-owned PHY and engine"

# Explicit firmware-engine parameters and request assignment.
copy_fixture sniffer-definition-parameter
perl -0pi -e 's/(mt7925_mcu_set_sniffer\([^;]*?bool enable),\n\s*u8 engine\n/$1\n/s' \
	"$tmp/sniffer-definition-parameter/mt7925/mcu.c"
sed -i '1s/fixture.h/mt792x.h/' \
	"$tmp/sniffer-definition-parameter/mt7925/mcu.c"
perl -0pi -e 's/(mt7925_mcu_set_sniffer\([^;]*?\)\n\{\n)/$1\tu8 engine = 0;\n/s' \
	"$tmp/sniffer-definition-parameter/mt7925/mcu.c"
expect_exact_diagnostics sniffer-definition-parameter \
	"$tmp/sniffer-definition-parameter" \
	"sniffer enable definition does not take an explicit firmware engine"

copy_fixture sniffer-declaration-parameter
perl -0pi -e 's/(mt7925_mcu_set_sniffer\([^;]*?bool enable),\n\s*u8 engine\n/$1\n/s' \
	"$tmp/sniffer-declaration-parameter/mt7925/mt7925.h"
expect_exact_diagnostics sniffer-declaration-parameter \
	"$tmp/sniffer-declaration-parameter" \
	"sniffer enable declaration does not take an explicit firmware engine"

copy_fixture sniffer-command-band
perl -0pi -e 's/(req\.hdr\.band_idx\s*=\s*)engine;/${1}0;/s' \
	"$tmp/sniffer-command-band/mt7925/mcu.c"
expect_exact_diagnostics sniffer-command-band "$tmp/sniffer-command-band" \
	"sniffer enable command does not use its explicit firmware engine"

copy_fixture sniffer-global-chandef
sed -i '/(void)dev;/a\	dev->sniffer_lifecycle.active_phy->chandef = 0;' \
	"$tmp/sniffer-global-chandef/mt7925/mcu.c"
expect_exact_diagnostics sniffer-global-chandef "$tmp/sniffer-global-chandef" \
	"sniffer command touches mutable global chandef state"

# Exact engine semantics, including unreachable and expression decoys.
for mutant in engine-2g engine-5g engine-6g; do
	copy_fixture "$mutant"
done
sed -i '/case NL80211_BAND_2GHZ:/,+1s/return 0/return 1/' \
	"$tmp/engine-2g/mt7925/main.c"
sed -i '/case NL80211_BAND_5GHZ:/,+2s/return 1/return 0/' \
	"$tmp/engine-5g/mt7925/main.c"
sed -i 's/case NL80211_BAND_6GHZ:/case NL80211_BAND_6GHZ:\n\t\treturn 0;\n\tcase 99:/' \
	"$tmp/engine-6g/mt7925/main.c"
for mutant in engine-2g engine-5g engine-6g; do
	expect_exact_diagnostics "$mutant" "$tmp/$mutant" \
		"engine mapping is not exactly 2.4 GHz to 0 and 5/6 GHz to 1"
done

copy_fixture engine-unreachable-return
sed -i '/switch (band) {/a\	if (0) return 0;' \
	"$tmp/engine-unreachable-return/mt7925/main.c"
sed -i '/case NL80211_BAND_2GHZ:/,+1s/return 0/break/' \
	"$tmp/engine-unreachable-return/mt7925/main.c"
expect_exact_diagnostics engine-unreachable-return \
	"$tmp/engine-unreachable-return" \
	"engine mapping is not exactly 2.4 GHz to 0 and 5/6 GHz to 1"

copy_fixture engine-expression-return
sed -i '/static int mt7927_band_to_engine/,/^}/c\
static int mt7927_band_to_engine(int band)\
{\
\treturn band == NL80211_BAND_2GHZ ? 0 : 1;\
}' "$tmp/engine-expression-return/mt7925/main.c"
expect_exact_diagnostics engine-expression-return \
	"$tmp/engine-expression-return" \
	"engine mapping is not exactly 2.4 GHz to 0 and 5/6 GHz to 1"

copy_fixture engine-if-equivalent
sed -i '/static int mt7927_band_to_engine/,/^}/c\
static int mt7927_band_to_engine(int band)\
{\
\tif (band == NL80211_BAND_2GHZ)\
\t\treturn 0;\
\tif (band == NL80211_BAND_5GHZ || band == NL80211_BAND_6GHZ)\
\t\treturn 1;\
\treturn -EINVAL;\
}' "$tmp/engine-if-equivalent/mt7925/main.c"
expect_pass engine-if-equivalent "$tmp/engine-if-equivalent"

# Same-engine helper closure.
copy_fixture same-engine-rearm
sed -i '/static int mt7927_same_engine_rearm/,/^}/s/err = mt7925_mcu_set_sniffer(dev, mconf->vif, true, engine);/err = mt7927_publish_band(dev, mconf, wcid);/' \
	"$tmp/same-engine-rearm/mt7925/main.c"
expect_exact_diagnostics same-engine-rearm "$tmp/same-engine-rearm" \
	"same-engine transition does not re-arm the sniffer"

copy_fixture same-engine-config
sed -i '/static int mt7927_same_engine_rearm/,/^}/c\
static int mt7927_same_engine_rearm(struct mt792x_dev *dev,\
\t\tstruct mt792x_bss_conf *mconf, struct mt76_wcid *wcid,\
\t\tint band_idx)\
{\
\tint err;\
\tu8 engine = (u8)mt7927_band_to_engine(band_idx);\
\t(void)wcid;\
\terr = mt7925_mcu_set_sniffer(dev, mconf->vif, true, engine);\
\tif (err < 0)\
\t\treturn err;\
\treturn mt7927_publish_band(dev, mconf, wcid);\
}' \
	"$tmp/same-engine-config/mt7925/main.c"
expect_exact_diagnostics same-engine-config "$tmp/same-engine-config" \
	"same-engine transition does not configure the sniffer"

copy_fixture same-engine-indirect-migration
sed -i '/static int mt7927_same_engine_rearm/,/^}/{/(void)wcid;/a\	(void)mt7927_new_dev_on(dev, mconf, wcid);
}' \
	"$tmp/same-engine-indirect-migration/mt7925/main.c"
expect_exact_diagnostics same-engine-indirect-migration \
	"$tmp/same-engine-indirect-migration" \
	"same-engine transition reaches DEV/BSS migration"

copy_fixture same-engine-direct
perl -0pi -e 's@if \(mt7927_same_engine\(mconf->mt76\.band_idx, band_idx\)\)\n\t\treturn mt7927_same_engine_rearm\(dev, mconf, wcid, band_idx\);@if (mt7927_same_engine(mconf->mt76.band_idx, band_idx)) {\n\t\tu8 engine = (u8)mt7927_band_to_engine(band_idx);\n\t\terr = mt7925_mcu_set_sniffer(dev, mconf->vif, true, engine);\n\t\tif (err < 0)\n\t\t\treturn err;\n\t\treturn mt7925_mcu_config_sniffer(mconf->vif, 0, engine);\n\t}@s' \
	"$tmp/same-engine-direct/mt7925/main.c"
expect_pass same-engine-direct "$tmp/same-engine-direct"

# Each forward stage must check its own result and immediately roll back.
forward_stages='old_sniffer_off old_bss_off old_dev_off new_dev_on new_bss_on new_sniffer_on new_sniffer_config publish_band'
for stage in $forward_stages; do
	name=forward-failure-$stage
	copy_fixture "$name"
	replace_marked_once "$tmp/$name/mt7925/main.c" \
		"forward:$stage" 'goto rollback' 'return err'
	expect_exact_diagnostics "$name" "$tmp/$name" \
		"forward stage $stage is not success-checked before journaling"
done

copy_fixture forward-journal-before-success
replace_marked_once "$tmp/forward-journal-before-success/mt7925/main.c" \
	'forward:new_dev_on' \
	'err = mt7927_new_dev_on(dev, mconf, wcid);' \
	'journal |= BIT(MT7927_JOURNAL_NEW_DEV_ON); err = mt7927_new_dev_on(dev, mconf, wcid);'
expect_exact_diagnostics forward-journal-before-success \
	"$tmp/forward-journal-before-success" \
	"forward stage new_dev_on is not success-checked before journaling"

copy_fixture forward-order
swap_adjacent_marked_lines "$tmp/forward-order/mt7925/main.c" \
	'forward:old_bss_off' 'forward:old_dev_off'
expect_exact_diagnostics forward-order "$tmp/forward-order" \
	"cross-engine forward stages are not in required order"

copy_fixture forward-success-no-resume
replace_marked_once "$tmp/forward-success-no-resume/mt7925/main.c" \
	'tx:forward-resume' 'mt76_worker_enable(&dev->mt76.tx_worker);' '(void)dev;'
expect_exact_diagnostics forward-success-no-resume \
	"$tmp/forward-success-no-resume" \
	"forward success does not resume TX"

copy_fixture forward-success-reset
replace_marked_once "$tmp/forward-success-reset/mt7925/main.c" \
	'tx:forward-resume' 'mt76_worker_enable(&dev->mt76.tx_worker);' \
	'queue_work(&reset_work); mt76_worker_enable(&dev->mt76.tx_worker);'
expect_exact_diagnostics forward-success-reset \
	"$tmp/forward-success-reset" \
	"forward success schedules reset"

copy_fixture tx-disable-wrong-worker
replace_marked_once "$tmp/tx-disable-wrong-worker/mt7925/main.c" \
	'tx:disable' '&dev->mt76.tx_worker' '0'
expect_exact_diagnostics tx-disable-wrong-worker \
	"$tmp/tx-disable-wrong-worker" \
	"cross-engine transaction does not quiesce and drain TX"

copy_fixture tx-drain-wrong-wait
replace_marked_once "$tmp/tx-drain-wrong-wait/mt7925/main.c" \
	'tx:drain-wait' 'dev->mt76.tx_wait' '0'
expect_exact_diagnostics tx-drain-wrong-wait \
	"$tmp/tx-drain-wrong-wait" \
	"cross-engine transaction does not quiesce and drain TX"

copy_fixture tx-drain-wrong-phy
replace_marked_once "$tmp/tx-drain-wrong-phy/mt7925/main.c" \
	'tx:drain-phy' '&dev->mphy' '0'
expect_exact_diagnostics tx-drain-wrong-phy \
	"$tmp/tx-drain-wrong-phy" \
	"cross-engine transaction does not quiesce and drain TX"

copy_fixture tx-forward-resume-wrong-worker
replace_marked_once "$tmp/tx-forward-resume-wrong-worker/mt7925/main.c" \
	'tx:forward-resume' '&dev->mt76.tx_worker' '0'
expect_exact_diagnostics tx-forward-resume-wrong-worker \
	"$tmp/tx-forward-resume-wrong-worker" \
	"forward success does not resume TX"

copy_fixture journal-init-nonzero
replace_marked_once "$tmp/journal-init-nonzero/mt7925/main.c" \
	'journal:init' 'journal = 0' 'journal = 1'
expect_exact_diagnostics journal-init-nonzero \
	"$tmp/journal-init-nonzero" \
	"cross-engine journal is not initialized and consistently threaded"

copy_fixture journal-forward-substitution
replace_marked_once "$tmp/journal-forward-substitution/mt7925/main.c" \
	'forward:new_dev_on' 'journal |= BIT(MT7927_JOURNAL_NEW_DEV_ON)' \
	'band_idx |= BIT(MT7927_JOURNAL_NEW_DEV_ON)'
expect_exact_diagnostics journal-forward-substitution \
	"$tmp/journal-forward-substitution" \
	"cross-engine journal is not initialized and consistently threaded"

copy_fixture journal-rollback-call-substitution
replace_marked_once "$tmp/journal-rollback-call-substitution/mt7925/main.c" \
	'journal:rollback-call' 'journal);' '(unsigned long)band_idx);'
expect_exact_diagnostics journal-rollback-call-substitution \
	"$tmp/journal-rollback-call-substitution" \
	"cross-engine journal is not initialized and consistently threaded"

copy_fixture journal-rollback-guard-substitution
replace_marked_once "$tmp/journal-rollback-guard-substitution/mt7925/main.c" \
	'journal:guard-new_bss_off' 'journal & BIT' 'err & BIT'
expect_exact_diagnostics journal-rollback-guard-substitution \
	"$tmp/journal-rollback-guard-substitution" \
	"cross-engine journal is not initialized and consistently threaded"

# Every reachable rollback stage must be guarded by its exact bit and checked.
rollback_stages='restore_band new_sniffer_unconfig new_sniffer_off new_bss_off new_dev_off old_dev_on old_bss_on old_sniffer_restore'
for stage in $rollback_stages; do
	name=rollback-failure-$stage
	copy_fixture "$name"
	replace_marked_once "$tmp/$name/mt7925/main.c" \
		"rollback:$stage" 'return err;' 'err = 0;'
	expect_exact_diagnostics "$name" "$tmp/$name" \
		"rollback stage $stage is not guarded and failure-checked"
done

copy_fixture rollback-wrong-bit
replace_marked_once "$tmp/rollback-wrong-bit/mt7925/main.c" \
	'journal:guard-new_bss_off' 'MT7927_JOURNAL_NEW_BSS_ON' \
	'MT7927_JOURNAL_NEW_DEV_ON'
expect_exact_diagnostics rollback-wrong-bit "$tmp/rollback-wrong-bit" \
	"rollback stage new_bss_off is not guarded and failure-checked"

copy_fixture rollback-order
swap_adjacent_marked_blocks_once "$tmp/rollback-order/mt7925/main.c" \
	'rollback:new_bss_off' 'rollback:new_dev_off'
expect_exact_diagnostics rollback-order "$tmp/rollback-order" \
	"cross-engine rollback stages are not in exact reverse order"

# Rollback result handling preserves the forward failure and gates reset/TX.
copy_fixture rollback-primary
replace_marked_once "$tmp/rollback-primary/mt7925/main.c" \
	'rollback:primary-save' 'primary_err = err;' 'primary_err = rollback_err;'
expect_exact_diagnostics rollback-primary "$tmp/rollback-primary" \
	"rollback path does not preserve the primary forward error"

copy_fixture rollback-primary-zero
replace_marked_once "$tmp/rollback-primary-zero/mt7925/main.c" \
	'rollback:primary-save' 'primary_err = err;' 'primary_err = 0;'
expect_exact_diagnostics rollback-primary-zero "$tmp/rollback-primary-zero" \
	"rollback path does not preserve the primary forward error"

copy_fixture rollback-primary-unrelated
replace_marked_once "$tmp/rollback-primary-unrelated/mt7925/main.c" \
	'rollback:primary-save' 'primary_err = err;' 'primary_err = band_idx;'
expect_exact_diagnostics rollback-primary-unrelated \
	"$tmp/rollback-primary-unrelated" \
	"rollback path does not preserve the primary forward error"

copy_fixture rollback-primary-mutated-forward-error
replace_marked_once "$tmp/rollback-primary-mutated-forward-error/mt7925/main.c" \
	'rollback:primary-save' 'primary_err = err;' \
	'err += 1; primary_err = err;'
expect_exact_diagnostics rollback-primary-mutated-forward-error \
	"$tmp/rollback-primary-mutated-forward-error" \
	"rollback path does not preserve the primary forward error"

copy_fixture rollback-primary-reassigned
replace_marked_once "$tmp/rollback-primary-reassigned/mt7925/main.c" \
	'journal:rollback-call' \
	'rollback_err = mt7927_reconfig_rollback(dev, mconf, wcid, journal);' \
	'rollback_err = mt7927_reconfig_rollback(dev, mconf, wcid, journal); primary_err = rollback_err;'
expect_exact_diagnostics rollback-primary-reassigned \
	"$tmp/rollback-primary-reassigned" \
	"rollback path does not preserve the primary forward error"

copy_fixture rollback-primary-compound-mutated
replace_marked_once "$tmp/rollback-primary-compound-mutated/mt7925/main.c" \
	'journal:rollback-call' \
	'rollback_err = mt7927_reconfig_rollback(dev, mconf, wcid, journal);' \
	'primary_err += 1; rollback_err = mt7927_reconfig_rollback(dev, mconf, wcid, journal);'
expect_exact_diagnostics rollback-primary-compound-mutated \
	"$tmp/rollback-primary-compound-mutated" \
	"rollback path does not preserve the primary forward error"

copy_fixture rollback-primary-incremented
replace_marked_once "$tmp/rollback-primary-incremented/mt7925/main.c" \
	'journal:rollback-call' \
	'rollback_err = mt7927_reconfig_rollback(dev, mconf, wcid, journal);' \
	'primary_err++; rollback_err = mt7927_reconfig_rollback(dev, mconf, wcid, journal);'
expect_exact_diagnostics rollback-primary-incremented \
	"$tmp/rollback-primary-incremented" \
	"rollback path does not preserve the primary forward error"

copy_fixture rollback-reset-success
replace_marked_once "$tmp/rollback-reset-success/mt7925/main.c" \
	'rollback:success-resume' 'mt76_worker_enable(&dev->mt76.tx_worker);' \
	'queue_work(&reset_work); mt76_worker_enable(&dev->mt76.tx_worker);'
expect_exact_diagnostics rollback-reset-success "$tmp/rollback-reset-success" \
	"rollback failure does not schedule reset exclusively"

copy_fixture rollback-reset-polarity
replace_marked_once "$tmp/rollback-reset-polarity/mt7925/main.c" \
	'rollback:failure-branch' 'if (rollback_err < 0)' \
	'if (rollback_err == 0)'
expect_exact_diagnostics rollback-reset-polarity \
	"$tmp/rollback-reset-polarity" \
	"rollback failure does not schedule reset exclusively"

copy_fixture rollback-failure-resume
replace_marked_once "$tmp/rollback-failure-resume/mt7925/main.c" \
	'rollback:failure-reset' 'queue_work(&reset_work);' \
	'queue_work(&reset_work); mt76_worker_enable(&dev->mt76.tx_worker);'
expect_exact_diagnostics rollback-failure-resume \
	"$tmp/rollback-failure-resume" \
	"rollback failure resumes TX"

copy_fixture rollback-failure-reset-after-return
replace_marked_once "$tmp/rollback-failure-reset-after-return/mt7925/main.c" \
	'rollback:failure-reset' 'queue_work(&reset_work);' \
	'return primary_err; queue_work(&reset_work);'
expect_exact_diagnostics rollback-failure-reset-after-return \
	"$tmp/rollback-failure-reset-after-return" \
	"rollback failure does not schedule reset exclusively"

copy_fixture rollback-failure-reset-dead-if
replace_marked_once "$tmp/rollback-failure-reset-dead-if/mt7925/main.c" \
	'rollback:failure-reset' 'queue_work(&reset_work);' \
	'if (0) queue_work(&reset_work);'
expect_exact_diagnostics rollback-failure-reset-dead-if \
	"$tmp/rollback-failure-reset-dead-if" \
	"rollback failure does not schedule reset exclusively"

copy_fixture rollback-success-no-resume
replace_marked_once "$tmp/rollback-success-no-resume/mt7925/main.c" \
	'rollback:success-resume' 'mt76_worker_enable(&dev->mt76.tx_worker);' \
	'(void)rollback_err;'
expect_exact_diagnostics rollback-success-no-resume \
	"$tmp/rollback-success-no-resume" \
	"successful rollback does not resume TX"

copy_fixture rollback-success-wrong-worker
replace_marked_once "$tmp/rollback-success-wrong-worker/mt7925/main.c" \
	'rollback:success-resume' '&dev->mt76.tx_worker' '0'
expect_exact_diagnostics rollback-success-wrong-worker \
	"$tmp/rollback-success-wrong-worker" \
	"successful rollback does not resume TX"

copy_fixture rollback-success-resume-after-return
replace_marked_once "$tmp/rollback-success-resume-after-return/mt7925/main.c" \
	'rollback:success-resume' 'mt76_worker_enable(&dev->mt76.tx_worker);' \
	'return primary_err; mt76_worker_enable(&dev->mt76.tx_worker);'
expect_exact_diagnostics rollback-success-resume-after-return \
	"$tmp/rollback-success-resume-after-return" \
	"successful rollback does not resume TX"

copy_fixture rollback-success-resume-dead-if
replace_marked_once "$tmp/rollback-success-resume-dead-if/mt7925/main.c" \
	'rollback:success-resume' 'mt76_worker_enable(&dev->mt76.tx_worker);' \
	'if (0) mt76_worker_enable(&dev->mt76.tx_worker);'
expect_exact_diagnostics rollback-success-resume-dead-if \
	"$tmp/rollback-success-resume-dead-if" \
	"successful rollback does not resume TX"

copy_fixture rollback-returns-success
replace_marked_once "$tmp/rollback-returns-success/mt7925/main.c" \
	'rollback:success-return' 'return primary_err;' 'return 0;'
expect_exact_diagnostics rollback-returns-success \
	"$tmp/rollback-returns-success" \
	"successful rollback does not return the primary forward error"

# Exact MLO link/mconf/WCID identity through the transaction and migration.
copy_fixture mlo-default-assignment
sed -i '0,/mt792x_vif_to_link_exact/s//mt792x_vif_to_link/' \
	"$tmp/mlo-default-assignment/mt7925/main.c"
expect_exact_diagnostics mlo-default-assignment \
	"$tmp/mlo-default-assignment" \
	"MLO assignment does not connect exact link and WCID to migration"

copy_fixture mlo-substituted-wcid
sed -i '/err = mt7927_reconfig_band/s/mconf, wcid/mconf, 0/' \
	"$tmp/mlo-substituted-wcid/mt7925/main.c"
expect_exact_diagnostics mlo-substituted-wcid "$tmp/mlo-substituted-wcid" \
	"MLO assignment does not connect exact link and WCID to migration"

copy_fixture mlo-address-of-assignment-wcid
replace_marked_once "$tmp/mlo-address-of-assignment-wcid/mt7925/main.c" \
	'mlo:assignment-wcid' 'wcid = mconf->mt76.wcid;' \
	'wcid = (struct mt76_wcid *)&mconf->mt76.wcid;'
expect_exact_diagnostics mlo-address-of-assignment-wcid \
	"$tmp/mlo-address-of-assignment-wcid" \
	"MLO assignment does not connect exact link and WCID to migration"

copy_fixture mlo-default-unassignment
sed -i '/static void mt7925_unassign_vif_chanctx/,/^}/s/mt792x_vif_to_link_exact/mt792x_vif_to_link/' \
	"$tmp/mlo-default-unassignment/mt7925/main.c"
expect_exact_diagnostics mlo-default-unassignment \
	"$tmp/mlo-default-unassignment" \
	"MLO unassignment does not connect exact link and WCID to migration"

copy_fixture mlo-address-of-unassignment-wcid
replace_marked_once "$tmp/mlo-address-of-unassignment-wcid/mt7925/main.c" \
	'mlo:unassignment-wcid' 'wcid = mconf->mt76.wcid;' \
	'wcid = (struct mt76_wcid *)&mconf->mt76.wcid;'
expect_exact_diagnostics mlo-address-of-unassignment-wcid \
	"$tmp/mlo-address-of-unassignment-wcid" \
	"MLO unassignment does not connect exact link and WCID to migration"

copy_fixture mlo-stage-substitution
sed -i '/forward:new_bss_on/s/dev, mconf, wcid/dev, mconf, 0/' \
	"$tmp/mlo-stage-substitution/mt7925/main.c"
expect_exact_diagnostics mlo-stage-substitution \
	"$tmp/mlo-stage-substitution" \
	"migration stages do not preserve exact mconf and WCID"

copy_fixture mlo-rollback-substitution
sed -i '/err = mt7927_old_dev_on/s/dev, mconf, wcid/dev, mconf, 0/' \
	"$tmp/mlo-rollback-substitution/mt7925/main.c"
expect_exact_diagnostics mlo-rollback-substitution \
	"$tmp/mlo-rollback-substitution" \
	"migration stages do not preserve exact mconf and WCID"

# Complete helper closure, including mt76_* helpers, rejects forbidden writes.
copy_fixture write-basic-rates
sed -i '/int mt7925_monitor_update_chan/i\
static void mt76_hidden_basic_rates(struct mt792x_vif *mvif) { mvif->basic_rates_idx = 1; }' \
	"$tmp/write-basic-rates/mt7925/main.c"
sed -i '/dev->sniffer_lifecycle.active_phy = phy;/i\	mt76_hidden_basic_rates(0);' \
	"$tmp/write-basic-rates/mt7925/main.c"
expect_exact_diagnostics write-basic-rates "$tmp/write-basic-rates" \
	"monitor lifecycle helper closure writes ordinary basic_rates_idx"

copy_fixture write-wcid-phy
sed -i '/(void)dev;/a\	wcid->phy_idx = 1;' \
	"$tmp/write-wcid-phy/mt7925/main.c"
expect_exact_diagnostics write-wcid-phy "$tmp/write-wcid-phy" \
	"monitor lifecycle helper closure writes ordinary wcid phy_idx"

copy_fixture write-global-chandef
sed -i '/(void)wcid;/a\	dev->sniffer_lifecycle.active_phy->chandef = 0;' \
	"$tmp/write-global-chandef/mt7925/main.c"
expect_exact_diagnostics write-global-chandef "$tmp/write-global-chandef" \
	"monitor lifecycle helper closure writes global PHY chandef"

# Legacy wrapper branches are exclusive, checked, and contain no extra stages.
copy_fixture legacy-enable-order
sed -i '/if (enable) {/,/^\t}/s/uni_add_dev_info/uni_add_stage_tmp/' \
	"$tmp/legacy-enable-order/mt76_connac_mcu.c"
sed -i '/if (enable) {/,/^\t}/s/uni_add_bss_info/uni_add_dev_info/' \
	"$tmp/legacy-enable-order/mt76_connac_mcu.c"
sed -i '/if (enable) {/,/^\t}/s/uni_add_stage_tmp/uni_add_bss_info/' \
	"$tmp/legacy-enable-order/mt76_connac_mcu.c"
expect_exact_diagnostics legacy-enable-order "$tmp/legacy-enable-order" \
	"legacy enable branch is not exclusively DEV-on then checked BSS-on"

copy_fixture legacy-enable-unchecked
sed -i '/if (enable) {/,/^\t}/s/if (err < 0)/if (err > 0)/' \
	"$tmp/legacy-enable-unchecked/mt76_connac_mcu.c"
expect_exact_diagnostics legacy-enable-unchecked \
	"$tmp/legacy-enable-unchecked" \
	"legacy enable branch is not exclusively DEV-on then checked BSS-on"

copy_fixture legacy-enable-polarity
replace_marked_once "$tmp/legacy-enable-polarity/mt76_connac_mcu.c" \
	'legacy:enable-branch' 'if (enable)' 'if (!enable)'
expect_exact_diagnostics legacy-enable-polarity \
	"$tmp/legacy-enable-polarity" \
	"legacy enable branch is not exclusively DEV-on then checked BSS-on"

copy_fixture legacy-disable-order
sed -i '/^\terr = mt76_connac_mcu_uni_add_bss_info/,/^\treturn mt76_connac_mcu_uni_add_dev_info/s/uni_add_bss_info/uni_add_stage_tmp/' \
	"$tmp/legacy-disable-order/mt76_connac_mcu.c"
sed -i '/^\terr = mt76_connac_mcu_uni_add_stage_tmp/,/^\treturn mt76_connac_mcu_uni_add_dev_info/s/uni_add_dev_info/uni_add_bss_info/' \
	"$tmp/legacy-disable-order/mt76_connac_mcu.c"
sed -i 's/uni_add_stage_tmp/uni_add_dev_info/' \
	"$tmp/legacy-disable-order/mt76_connac_mcu.c"
expect_exact_diagnostics legacy-disable-order "$tmp/legacy-disable-order" \
	"legacy disable branch is not exclusively BSS-off then DEV-off"

for position in before between after; do
	name=legacy-extra-$position
	copy_fixture "$name"
	case $position in
	before)
		sed -i '/if (enable) {/a\	\t(void)mt76_connac_mcu_uni_add_bss_info(mconf, wcid, false);' \
			"$tmp/$name/mt76_connac_mcu.c"
		;;
	between)
		sed -i '/if (enable) {/,/^\t}/{/if (err < 0)/a\	\t(void)mt76_connac_mcu_uni_add_dev_info(mconf, wcid, true);
}' \
			"$tmp/$name/mt76_connac_mcu.c"
		;;
	after)
		sed -i '/return mt76_connac_mcu_uni_add_bss_info/i\	\t(void)mt76_connac_mcu_uni_add_dev_info(mconf, wcid, true);' \
			"$tmp/$name/mt76_connac_mcu.c"
		;;
	esac
	expect_exact_diagnostics "$name" "$tmp/$name" \
		"legacy enable branch is not exclusively DEV-on then checked BSS-on"
done

copy_fixture legacy-fallthrough
sed -i 's/return mt76_connac_mcu_uni_add_bss_info(mconf, wcid, true);/(void)mt76_connac_mcu_uni_add_bss_info(mconf, wcid, true);/' \
	"$tmp/legacy-fallthrough/mt76_connac_mcu.c"
expect_exact_diagnostics legacy-fallthrough "$tmp/legacy-fallthrough" \
	"legacy enable branch is not exclusively DEV-on then checked BSS-on"

# Batch migration remains atomic with publication delayed until all members pass.
copy_fixture switch-batch
sed -i 's/n_vifs <= 0/n_vifs < 0/' "$tmp/switch-batch/mt7925/main.c"
expect_exact_diagnostics switch-batch "$tmp/switch-batch" \
	"chanctx switch does not validate batch and mode"

copy_fixture switch-first-only
sed -i '0,/vifs\[i\]\.mconf/s//vifs[0].mconf/' \
	"$tmp/switch-first-only/mt7925/main.c"
expect_exact_diagnostics switch-first-only "$tmp/switch-first-only" \
	"chanctx switch does not migrate every member before publication"

copy_fixture switch-publish-early
sed -i '/for (i = 0; i < n_vifs; i++) {/a\	\tvifs[i].mconf->mt76.ctx = vifs[i].new_ctx;' \
	"$tmp/switch-publish-early/mt7925/main.c"
expect_exact_diagnostics switch-publish-early "$tmp/switch-publish-early" \
	"chanctx switch does not migrate every member before publication"

copy_fixture switch-unlock-early
sed -i '/for (i = 0; i < n_vifs; i++)$/i\	mutex_unlock(\&dev->mutex);' \
	"$tmp/switch-unlock-early/mt7925/main.c"
expect_exact_diagnostics switch-unlock-early "$tmp/switch-unlock-early" \
	"chanctx switch does not hold one mutex across migration and publication"

if [ "$selftest_failures" -ne 0 ]; then
	printf 'FAIL: MT7927 driver lifecycle contract self-test (%s failures)\n' \
		"$selftest_failures" >&2
	exit 1
fi
printf 'PASS: MT7927 driver lifecycle contract self-test\n'
