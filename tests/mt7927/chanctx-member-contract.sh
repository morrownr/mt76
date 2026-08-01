#!/bin/sh
set -eu

root=${1:?usage: chanctx-member-contract.sh path/to/mt76}
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

my ($body) = $source =~ /
\bmt7927_change_chanctx_link\s*\([^;]*?\)\s*\{(.*?)\}
\s*static\s+void\s+mt7927_change_chanctx_iter\b
/x;
die "FAIL: MT7927 chanctx member helper is missing\n" unless defined $body;

die "FAIL: monitor ownership is not matched by lifecycle owner and link\n"
	unless $body =~ /monitor\s*=.*?state\s*==\s*MT792X_MONITOR_ACTIVE.*?
	owner\s*==\s*vif.*?link_id\s*==\s*link_conf\s*->\s*link_id\s*;/x;

die "FAIL: monitor owner does not use the engine transaction\n"
	unless $body =~ /if\s*\(\s*monitor\s*\).*?mt7927_reconfig_band\s*\(/;

die "FAIL: ordinary MT7927 chanctx member bypasses set_chctx\n"
	unless $body =~ /mt7925_mcu_set_chctx\s*\(/;

die "FAIL: ordinary MT7927 puncturing update is missing\n"
	unless $body =~ /IEEE80211_CHANCTX_CHANGE_PUNCTURING.*?
	mt7925_mcu_set_eht_pp\s*\(/x;

print "PASS: MT7927 chanctx member contract\n";
PERL
