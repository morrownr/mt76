#!/bin/sh
set -eu

root=${1:?usage: void-callback-recovery-contract.sh path/to/mt76}
main=$root/mt7925/main.c

perl - "$main" <<'PERL'
use strict;
use warnings;

my $path = shift @ARGV;
open my $fh, '<', $path or die "cannot read $path: $!\n";
local $/;
my $source = <$fh>;

$source =~ s{\/\*.*?\*\/}{ }gs;
$source =~ s{//[^\n]*}{ }g;
$source =~ s/\s+/ /g;

die "FAIL: no common void-callback recovery helper\n"
	unless $source =~ /
	\bmt7925_chanctx_failure\s*\([^;]*?\)\s*\{.*?
	\bdev_err\s*\(.*?\)\s*;.*?
	\bmt792x_reset\s*\(\s*&\s*dev\s*->\s*mt76\s*\)\s*;.*?\}
	/x;

my ($sniffer) = $source =~ /
\bmt7925_sniffer_interface_iter\s*\([^;]*?\)\s*\{(.*?)\}
\s*void\s+mt7925_set_runtime_pm\b
/x;
die "FAIL: sniffer configuration failure has no disable rollback\n"
	unless defined $sniffer &&
	       $sniffer =~ /mt7925_mcu_config_sniffer\s*\(.*?
	       if\s*\(\s*err\s*<\s*0\s*\)\s*\{.*?
	       mt7925_mcu_set_sniffer\s*\([^;]*?false[^;]*?\).*?
	       reset_required/x;

my ($change) = $source =~ /
\bmt7925_change_chanctx\s*\([^;]*?\)\s*\{(.*?)\}
\s*static\s+void\s+mt7925_mgd_prepare_tx\b
/x;
die "FAIL: change_chanctx does not reset after an unrecoverable error\n"
	unless defined $change &&
	       $change =~ /\bmt7925_chanctx_failure\s*\(\s*[^,]+\s*,\s*[^,]+\s*,\s*[^)]+\)/;

my ($unassign) = $source =~ /
\bmt7925_unassign_vif_chanctx\s*\([^;]*?\)\s*\{(.*?)\}
\s*static\s+void\s+mt7925_rfkill_poll\b
/x;
die "FAIL: unassign_vif_chanctx does not reset after an unrecoverable error\n"
	unless defined $unassign &&
	       $unassign =~ /\bmt7925_chanctx_failure\s*\(\s*[^,]+\s*,\s*[^,]+\s*,\s*[^)]+\)/;

my ($filter) = $source =~ /
\bmt7925_configure_filter\s*\([^;]*?\)\s*\{(.*?)\}
\s*static\s+u8\s+mt7925_get_rates_table\b
/x;
die "FAIL: configure_filter does not reset after an unrecoverable error\n"
	unless defined $filter &&
	       $filter =~ /\bmt7925_chanctx_failure\s*\(\s*[^,]+\s*,\s*[^,]+\s*,\s*[^)]+\)/;

print "PASS: MT7927 void callback recovery contract\n";
PERL
