#!/bin/sh
set -eu

root=${1:?usage: monitor-owner-lifetime-contract.sh path/to/mt76}
main=$root/mt7925/main.c

perl - "$main" <<'PERL'
use strict;
use warnings;

my $path = shift @ARGV;
open my $fh, '<', $path or die "cannot read $path: $!\n";
local $/;
my $source = <$fh>;

$source =~ s{/\*.*?\*/}{ }gs;
$source =~ s{//[^\n]*}{ }g;
$source =~ s/\s+/ /g;

die "FAIL: monitor lifecycle has no complete reset helper\n"
	unless $source =~ /
	\bmt7925_monitor_reset\s*\([^;]*?\)\s*\{
	.*?\*\s*lifecycle\s*=\s*
	\(\s*struct\s+mt792x_monitor_lifecycle\s*\)\s*\{\s*\}\s*;
	.*?\}
	/x;

my ($remove) = $source =~ /
\bmt7925_remove_interface\s*\([^;]*?\)\s*\{(.*?)\}
\s*static\s+void\s+mt7925_roc_iter\b
/x;
die "FAIL: interface removal does not retire device-owned monitor state\n"
	unless defined $remove &&
	       $remove =~ /monitor\s*\.\s*owner\s*==\s*vif/ &&
	       $remove =~ /mt7925_monitor_reset\s*\(/ &&
	       $remove =~ /mt792x_remove_interface\s*\(\s*hw\s*,\s*vif\s*\)/;

die "FAIL: mt7925 ops bypass monitor-aware interface removal\n"
	unless $source =~ /\.\s*remove_interface\s*=\s*mt7925_remove_interface\s*,/;

my ($candidate) = $source =~ /
\bmt7925_monitor_owner_candidate\s*\([^;]*?\)\s*\{(.*?)\}
\s*static\s+void\s+mt7925_sniffer_interface_iter\b
/x;
die "FAIL: monitor owner candidate selector is missing\n"
	unless defined $candidate;
die "FAIL: initial monitor owner is not restricted to a monitor VIF\n"
	unless $candidate =~ /state\s*==\s*MT792X_MONITOR_OFF.*?
		vif\s*->\s*type\s*==\s*NL80211_IFTYPE_MONITOR/x;
die "FAIL: established monitor ownership does not follow lifecycle state\n"
	unless $candidate =~ /owner\s*==\s*vif/;

my ($sniffer) = $source =~ /
\bmt7925_sniffer_interface_iter\s*\([^;]*?\)\s*\{(.*?)\}
\s*void\s+mt7925_set_runtime_pm\b
/x;
die "FAIL: sniffer iteration does not enforce monitor ownership\n"
	unless defined $sniffer &&
	       $sniffer =~ /mt7925_monitor_owner_candidate\s*\(\s*lifecycle\s*,\s*vif\s*\)/;

print "PASS: MT7927 monitor owner lifetime contract\n";
PERL
