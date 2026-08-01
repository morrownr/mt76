#!/bin/sh
set -eu

root=${1:?usage: monitor-owner-scope-contract.sh path/to/mt76}
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

my ($body) = $source =~ /
\bmt7927_reconfig_band\s*\([^;]*?\)\s*\{(.*?)
\}\s*static\s+int\s+mt7925_assign_vif_chanctx\b
/x;
die "FAIL: MT7927 reconfiguration transaction not found\n" unless defined $body;

my ($assignment) = $body =~ /
\bdata\s*\.\s*old_sniffer_active\s*=\s*(.*?);
/x;
die "FAIL: MT7927 transaction does not derive old sniffer activity\n"
	unless defined $assignment;

my @required = (
	qr/\bdata\s*\.\s*old_monitor\s*\.\s*state\s*==\s*MT792X_MONITOR_ACTIVE\b/,
	qr/\bdata\s*\.\s*old_monitor\s*\.\s*owner\s*==\s*vif\b/,
	qr/\bdata\s*\.\s*old_monitor\s*\.\s*link_id\s*==\s*link_conf\s*->\s*link_id\b/,
);

for my $required (@required) {
	die "FAIL: active sniffer ownership is not scoped to the current VIF and link\n"
		unless $assignment =~ $required;
}

print "PASS: MT7927 monitor owner scope contract\n";
PERL
