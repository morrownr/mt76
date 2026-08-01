#!/bin/sh
set -eu

root=${1:?usage: driver-lifecycle-contract.sh path/to/mt76}

exec perl - "$root" <<'PERL'
use strict;
use warnings;

my $root = shift @ARGV;
my %paths = (
	main => "$root/mt7925/main.c",
	mcu => "$root/mt7925/mcu.c",
	header => "$root/mt7925/mt7925.h",
	common => "$root/mt792x.h",
	connac => "$root/mt76_connac_mcu.c",
);
my @failures;
my (%tokens, %functions, %declarations, %structs);

sub fail_contract
{
	push @failures, $_[0];
}

sub read_file
{
	my ($path) = @_;
	open my $fh, '<', $path or return;
	local $/;
	return <$fh>;
}

sub mask_c_source
{
	my ($source) = @_;

	# Translation phase 2 precedes comment recognition in phase 3.
	$source =~ s/\\\r?\n//g;
	my $masked = '';
	my $state = 'code';
	my $length = length $source;
	my $i = 0;
	while ($i < $length) {
		my $c = substr($source, $i, 1);
		my $next = $i + 1 < $length ? substr($source, $i + 1, 1) : '';
		if ($state eq 'code') {
			if ($c eq '/' && $next eq '*') {
				$masked .= '  ';
				$i += 2;
				$state = 'block_comment';
				next;
			}
			if ($c eq '/' && $next eq '/') {
				$masked .= '  ';
				$i += 2;
				$state = 'line_comment';
				next;
			}
			if ($c eq '"' || $c eq "'") {
				$state = $c eq '"' ? 'string' : 'character';
				$masked .= ' ';
				$i++;
				next;
			}
			$masked .= $c;
			$i++;
			next;
		}
		if ($state eq 'block_comment') {
			if ($c eq '*' && $next eq '/') {
				$masked .= '  ';
				$i += 2;
				$state = 'code';
			} else {
				$masked .= $c eq "\n" ? "\n" : ' ';
				$i++;
			}
			next;
		}
		if ($state eq 'line_comment') {
			if ($c eq "\n") {
				$masked .= "\n";
				$state = 'code';
			} else {
				$masked .= ' ';
			}
			$i++;
			next;
		}
		if ($c eq "\n") {
			# An unspliced newline ends an invalid literal without hiding later code.
			$masked .= "\n";
			$state = 'code';
			$i++;
			next;
		}
		if ($c eq '\\') {
			$masked .= ' ';
			$i++;
			if ($i < $length) {
				$masked .= substr($source, $i, 1) eq "\n" ? "\n" : ' ';
				$i++;
			}
			next;
		}
		my $closes = ($state eq 'string' && $c eq '"') ||
			     ($state eq 'character' && $c eq "'");
		$masked .= ' ';
		$i++;
		$state = 'code' if $closes;
	}

	# Strings/comments are spaces now, so # is the first preprocessing token.
	$masked =~ s{^([ \t]*\#[^\n]*)(\n|\z)}{(' ' x length($1)) . $2}mge;
	return $masked;
}

sub tokenize
{
	my ($source) = @_;
	my @result;
	pos($source) = 0;
	while (pos($source) < length($source)) {
		if ($source =~ /\G\s+/gc) {
			next;
		}
		if ($source =~ /\G([A-Za-z_]\w*|0[xX][0-9A-Fa-f]+|[0-9]+|\.\.\.|>>=|<<=|==|!=|<=|>=|\+\+|--|->|&&|\|\||<<|>>|\+=|-=|\*=|\/=|%=|&=|\|=|\^=|##|.)/gcs) {
			push @result, $1;
			next;
		}
		pos($source)++;
	}
	return \@result;
}

sub matching
{
	my ($items, $start, $open, $close) = @_;
	my $depth = 0;
	for (my $i = $start; $i < @$items; $i++) {
		$depth++ if $items->[$i] eq $open;
		if ($items->[$i] eq $close) {
			$depth--;
			return $i if $depth == 0;
		}
	}
	return;
}

sub text_of
{
	my ($items) = @_;
	return join ' ', @$items;
}

sub slice
{
	my ($items, $first, $last) = @_;
	return [] if !defined($first) || !defined($last) || $last < $first;
	return [ @$items[$first .. $last] ];
}

sub parse_unit
{
	my ($unit, $items) = @_;
	my $depth = 0;
	for (my $i = 0; $i < @$items; $i++) {
		my $token = $items->[$i];
		if ($depth == 0 && $token eq 'struct' &&
			$i + 2 < @$items && $items->[$i + 1] =~ /^\w+$/ &&
			$items->[$i + 2] eq '{') {
			my $end = matching($items, $i + 2, '{', '}');
			if (defined $end) {
				$structs{$items->[$i + 1]} = {
					unit => $unit,
					body => slice($items, $i + 3, $end - 1),
				};
			}
		}
		if ($depth == 0 && $token eq '(' && $i > 0 &&
			$items->[$i - 1] =~ /^[A-Za-z_]\w*$/) {
			my $name = $items->[$i - 1];
			my $close = matching($items, $i, '(', ')');
			if (defined $close) {
				my $end = $close + 1;
				my $saw_assignment = 0;
				while ($end < @$items && $items->[$end] ne ';' &&
				       $items->[$end] ne '{') {
					$saw_assignment = 1 if $items->[$end] eq '=';
					$end++;
				}
				my $params = slice($items, $i + 1, $close - 1);
				if ($end < @$items && !$saw_assignment &&
				    $items->[$end] eq ';') {
					$declarations{$unit}{$name} //= { params => $params };
				}
				if ($end < @$items && !$saw_assignment &&
				    $items->[$end] eq '{') {
					my $body_end = matching($items, $end, '{', '}');
					if (defined $body_end) {
						$functions{$name} = {
							unit => $unit,
							params => $params,
							body => slice($items, $end + 1, $body_end - 1),
						};
						$i = $body_end;
						next;
					}
				}
			}
		}
		$depth++ if $token eq '{';
		$depth-- if $token eq '}' && $depth > 0;
	}
}

sub split_top_level
{
	my ($items, $separator) = @_;
	my (@parts, @part);
	my ($paren, $brace, $bracket) = (0, 0, 0);
	for my $token (@$items) {
		if ($token eq $separator && !$paren && !$brace && !$bracket) {
			push @parts, [@part];
			@part = ();
			next;
		}
		push @part, $token;
		$paren++ if $token eq '(';
		$paren-- if $token eq ')';
		$brace++ if $token eq '{';
		$brace-- if $token eq '}';
		$bracket++ if $token eq '[';
		$bracket-- if $token eq ']';
	}
	push @parts, [@part] if @part || !@parts;
	return @parts;
}

sub parameter_names
{
	my ($function) = @_;
	return () unless $function;
	my @names;
	for my $part (split_top_level($function->{params}, ',')) {
		my @identifiers = grep { /^[A-Za-z_]\w*$/ } @$part;
		push @names, @identifiers ? $identifiers[-1] : '';
	}
	return @names;
}

sub parameter_parts
{
	my ($function) = @_;
	return () unless $function;
	return split_top_level($function->{params}, ',');
}

sub calls_in
{
	my ($items) = @_;
	my @calls;
	my %keywords = map { $_ => 1 } qw(if for while switch return sizeof typeof);
	for (my $i = 0; $i + 1 < @$items; $i++) {
		next unless $items->[$i] =~ /^[A-Za-z_]\w*$/ &&
			$items->[$i + 1] eq '(' && !$keywords{$items->[$i]};
		my $end = matching($items, $i + 1, '(', ')');
		next unless defined $end;
		my $inside = slice($items, $i + 2, $end - 1);
		push @calls, {
			name => $items->[$i],
			args => [ split_top_level($inside, ',') ],
			position => $i,
		};
	}
	return @calls;
}

sub calls_named
{
	my ($function, $name) = @_;
	return () unless $function;
	return grep { $_->{name} eq $name } calls_in($function->{body});
}

sub top_level_calls_in
{
	my ($items) = @_;
	my (@calls, $depth);
	my %keywords = map { $_ => 1 } qw(if for while switch return sizeof typeof);
	for (my $i = 0; $i + 1 < @$items; $i++) {
		if ($items->[$i] eq '{') {
			$depth++;
			next;
		}
		if ($items->[$i] eq '}') {
			$depth-- if $depth;
			next;
		}
		next if $depth || $items->[$i] !~ /^[A-Za-z_]\w*$/ ||
			$items->[$i + 1] ne '(' || $keywords{$items->[$i]};
		my $end = matching($items, $i + 1, '(', ')');
		next unless defined $end;
		my $inside = slice($items, $i + 2, $end - 1);
		push @calls, {
			name => $items->[$i],
			args => [ split_top_level($inside, ',') ],
			position => $i,
		};
	}
	return @calls;
}

sub top_level_returns_in
{
	my ($items) = @_;
	my (@returns, $depth);
	for (my $i = 0; $i < @$items; $i++) {
		if ($items->[$i] eq '{') {
			$depth++;
			next;
		}
		if ($items->[$i] eq '}') {
			$depth-- if $depth;
			next;
		}
		next if $depth || $items->[$i] ne 'return';
		my $end = $i + 1;
		$end++ while $end < @$items && $items->[$end] ne ';';
		next if $end >= @$items;
		push @returns, {
			value => text_of(slice($items, $i + 1, $end - 1)),
			position => $i,
		};
	}
	return @returns;
}

sub has_top_level_control
{
	my ($items) = @_;
	my $depth = 0;
	for my $item (@$items) {
		if ($item eq '{') {
			$depth++;
			next;
		}
		if ($item eq '}') {
			$depth-- if $depth;
			next;
		}
		return 1 if !$depth && $item =~ /^(?:if|for|while|switch|do|goto)$/;
	}
	return 0;
}

sub writes_to_var
{
	my ($text, $var) = @_;
	my $writes = () = $text =~ /\b\Q$var\E\s*(?:<<=|>>=|\+=|-=|\*=|\/=|%=|&=|\|=|\^=|=(?!=))/g;
	$writes += () = $text =~ /\b\Q$var\E\s*(?:\+\+|--)/g;
	$writes += () = $text =~ /(?:\+\+|--)\s*\b\Q$var\E\b/g;
	return $writes;
}

sub closure
{
	my (@roots) = @_;
	my (%seen, @pending, @ordered);
	@pending = grep { exists $functions{$_} } @roots;
	while (@pending) {
		my $name = shift @pending;
		next if $seen{$name}++;
		push @ordered, $name;
		for my $call (calls_in($functions{$name}{body})) {
			push @pending, $call->{name}
				if exists $functions{$call->{name}} && !$seen{$call->{name}};
		}
	}
	return @ordered;
}

sub statement_range
{
	my ($items, $start) = @_;
	return unless defined $start && $start < @$items;
	if ($items->[$start] eq '{') {
		my $end = matching($items, $start, '{', '}');
		return ($start + 1, $end - 1, $end + 1) if defined $end;
		return;
	}
	my ($paren, $bracket) = (0, 0);
	for (my $i = $start; $i < @$items; $i++) {
		$paren++ if $items->[$i] eq '(';
		$paren-- if $items->[$i] eq ')';
		$bracket++ if $items->[$i] eq '[';
		$bracket-- if $items->[$i] eq ']';
		return ($start, $i, $i + 1)
			if $items->[$i] eq ';' && !$paren && !$bracket;
	}
	return;
}

sub top_if
{
	my ($function, $wanted_call) = @_;
	return unless $function;
	my $items = $function->{body};
	my $depth = 0;
	for (my $i = 0; $i < @$items; $i++) {
		$depth++ if $items->[$i] eq '{';
		$depth-- if $items->[$i] eq '}';
		next unless !$depth && $items->[$i] eq 'if' &&
			$i + 1 < @$items && $items->[$i + 1] eq '(';
		my $condition_end = matching($items, $i + 1, '(', ')');
		next unless defined $condition_end;
		my $condition = slice($items, $i + 2, $condition_end - 1);
		next if defined($wanted_call) &&
			text_of($condition) !~ /\b\Q$wanted_call\E\s*\(/;
		my ($first, $last, $after) = statement_range($items, $condition_end + 1);
		return {
			condition => $condition,
			body => slice($items, $first, $last),
			after => $after,
			if_position => $i,
		};
	}
	return;
}

sub direct_switch_map
{
	my ($function) = @_;
	return 0 unless $function;
	my @params = parameter_names($function);
	return 0 unless @params;
	my $items = $function->{body};
	my ($switch, $open, $close);
	my $depth = 0;
	for (my $i = 0; $i < @$items; $i++) {
		$depth++ if $items->[$i] eq '{';
		$depth-- if $items->[$i] eq '}';
		next unless !$depth && $items->[$i] eq 'switch' &&
			$i + 1 < @$items && $items->[$i + 1] eq '(';
		my $condition_end = matching($items, $i + 1, '(', ')');
		next unless defined $condition_end;
		my $condition = text_of(slice($items, $i + 2, $condition_end - 1));
		next unless $condition eq $params[0];
		next unless $condition_end + 1 < @$items &&
			$items->[$condition_end + 1] eq '{';
		$switch = $condition_end + 1;
		$close = matching($items, $switch, '{', '}');
		last;
	}
	return 0 unless defined $switch && defined $close;
	my (%labels, @all_labels);
	$depth = 0;
	for (my $i = $switch + 1; $i < $close; $i++) {
		$depth++ if $items->[$i] eq '{';
		$depth-- if $items->[$i] eq '}';
		next unless !$depth && ($items->[$i] eq 'case' ||
			$items->[$i] eq 'default');
		my $key = $items->[$i] eq 'default' ? 'default' : $items->[$i + 1];
		my $colon = $i + ($items->[$i] eq 'default' ? 1 : 2);
		next unless $colon < $close && $items->[$colon] eq ':';
		$labels{$key} = $colon + 1;
		push @all_labels, $key;
	}
	my %result;
	for my $key (@all_labels) {
		my $start = $labels{$key};
		my $local_depth = 0;
		for (my $i = $start; $i < $close; $i++) {
			$local_depth++ if $items->[$i] eq '{';
			$local_depth-- if $items->[$i] eq '}';
			next if $local_depth;
			last if $items->[$i] eq 'break';
			if ($items->[$i] eq 'return') {
				my $end = $i + 1;
				$end++ while $end < $close && $items->[$end] ne ';';
				$result{$key} = text_of(slice($items, $i + 1, $end - 1));
				last;
			}
		}
	}
	return 0 unless ($result{NL80211_BAND_2GHZ} // '') eq '0' &&
		($result{NL80211_BAND_5GHZ} // '') eq '1' &&
		($result{NL80211_BAND_6GHZ} // '') eq '1';
	for my $key (@all_labels) {
		next if $key eq 'NL80211_BAND_2GHZ' ||
			$key eq 'NL80211_BAND_5GHZ' ||
			$key eq 'NL80211_BAND_6GHZ';
		return 0 if ($result{$key} // '') eq '0' || ($result{$key} // '') eq '1';
	}
	return 1;
}

sub strip_outer_parentheses
{
	my ($items) = @_;
	while (@$items >= 2 && $items->[0] eq '(' && $items->[-1] eq ')') {
		my $end = matching($items, 0, '(', ')');
		last unless defined($end) && $end == $#$items;
		$items = slice($items, 1, $#$items - 1);
	}
	return $items;
}

sub equality_band
{
	my ($items, $parameter) = @_;
	$items = strip_outer_parentheses($items);
	return unless @$items == 3 && $items->[1] eq '==';
	return $items->[2] if $items->[0] eq $parameter &&
		$items->[2] =~ /^NL80211_BAND_(?:2|5|6)GHZ$/;
	return $items->[0] if $items->[2] eq $parameter &&
		$items->[0] =~ /^NL80211_BAND_(?:2|5|6)GHZ$/;
	return;
}

sub direct_if_map
{
	my ($function) = @_;
	return 0 unless $function;
	my @params = parameter_names($function);
	return 0 unless @params;
	my $parameter = $params[0];
	my $items = $function->{body};
	my (%result, $default_seen);
	my $i = 0;
	while ($i < @$items) {
		if ($items->[$i] eq 'if' && $i + 1 < @$items &&
		    $items->[$i + 1] eq '(') {
			my $condition_end = matching($items, $i + 1, '(', ')');
			return 0 unless defined $condition_end;
			my $condition = slice($items, $i + 2, $condition_end - 1);
			my @parts = split_top_level($condition, '||');
			my @bands;
			for my $part (@parts) {
				my $band = equality_band($part, $parameter);
				return 0 unless defined $band;
				push @bands, $band;
			}
			my ($first, $last, $after) =
				statement_range($items, $condition_end + 1);
			return 0 unless defined($first) && defined($last);
			my $statement = slice($items, $first, $last);
			return 0 unless @$statement == 3 &&
				$statement->[0] eq 'return' &&
				$statement->[1] =~ /^[01]$/ &&
				$statement->[2] eq ';';
			for my $band (@bands) {
				return 0 if exists $result{$band};
				$result{$band} = $statement->[1];
			}
			$i = $after;
			next;
		}
		if ($items->[$i] eq 'return') {
			my $end = $i + 1;
			$end++ while $end < @$items && $items->[$end] ne ';';
			return 0 if $end >= @$items || $default_seen;
			my $value = text_of(slice($items, $i + 1, $end - 1));
			return 0 if $value eq '0' || $value eq '1';
			$default_seen = 1;
			$i = $end + 1;
			next;
		}
		return 0;
	}
	return $default_seen &&
		($result{NL80211_BAND_2GHZ} // '') eq '0' &&
		($result{NL80211_BAND_5GHZ} // '') eq '1' &&
		($result{NL80211_BAND_6GHZ} // '') eq '1' &&
		scalar(keys %result) == 3;
}

for my $unit (qw(main mcu header common connac)) {
	my $path = $paths{$unit};
	my $source = read_file($path);
	if (!defined $source) {
		fail_contract("missing source file: $path");
		next;
	}
	$tokens{$unit} = tokenize(mask_c_source($source));
	parse_unit($unit, $tokens{$unit});
}

if (@failures) {
	print STDERR "FAIL: $_\n" for @failures;
	printf STDERR "FAIL: MT7927 driver lifecycle contract (%d violations)\n",
		scalar @failures;
	exit 1;
}

# Device-owned monitor state is discovered by type and use, not spelling.
my ($state_name, $state_member, $owner_member, $engine_member);
if (my $dev_struct = $structs{mt792x_dev}) {
	my $dev_text = text_of($dev_struct->{body});
	STATE:
	for my $candidate (sort keys %structs) {
		my $body = text_of($structs{$candidate}{body});
		next unless $body =~ /\bstruct\s+mt792x_phy\s*\*\s*(\w+)\s*;/;
		my $owner = $1;
		next unless $body =~ /\b(?:u8|u16|u32|unsigned\s+int|int)\s+(\w+)\s*;/;
		my $engine = $1;
		next unless $dev_text =~ /\bstruct\s+\Q$candidate\E\s+(\w+)\s*;/;
		($state_name, $state_member, $owner_member, $engine_member) =
			($candidate, $1, $owner, $engine);
		last STATE;
	}
}
fail_contract('monitor lifecycle state has no device-owned PHY and engine')
	unless defined $state_name;

my $monitor_sync = $functions{mt7925_monitor_sync};
my $monitor_update = $functions{mt7925_monitor_update_chan};
if (defined $state_name && $monitor_sync) {
	my @params = parameter_names($monitor_sync);
	my $dev = $params[0] // 'dev';
	my $sync_text = text_of($monitor_sync->{body});
	my $owner_path = quotemeta "$dev -> $state_member . $owner_member";
	my $engine_path = quotemeta "$dev -> $state_member . $engine_member";
	if ($sync_text !~ /$owner_path/ || $sync_text !~ /$engine_path/) {
		fail_contract('monitor reconciliation does not consume device-owned PHY and engine');
	}
	if ($monitor_update) {
		my @update_params = parameter_names($monitor_update);
		my $update_dev = $update_params[0] // 'dev';
		my $update_text = text_of($monitor_update->{body});
		my $update_owner = quotemeta "$update_dev -> $state_member . $owner_member";
		my $update_engine = quotemeta "$update_dev -> $state_member . $engine_member";
		if ($update_text !~ /$update_owner\s*=/ ||
		    $update_text !~ /$update_engine\s*=/) {
			fail_contract('monitor update does not publish device-owned PHY and engine');
		}
	} else {
		fail_contract('monitor update does not publish device-owned PHY and engine');
	}
} else {
	# The missing-state diagnostic is the root cause; dependent use checks
	# would only add collateral output for the same defect.
}

if (my $dev_struct = $structs{mt792x_dev}) {
	my $dev_text = text_of($dev_struct->{body});
	my $sync_text = $monitor_sync ? text_of($monitor_sync->{body}) : '';
	while ($dev_text =~ /\bstruct\s+mt792x_bss_conf\s*\*\s*(\w+)\s*;/g) {
		my $member = $1;
		if ($sync_text =~ /->\s*\Q$member\E\b/) {
			fail_contract('monitor lifecycle state makes shared BSS context authoritative');
			last;
		}
	}
}

sub check_sniffer_function
{
	my ($name, $definition_message, $declaration_message, $command_message,
		$expected_count) = @_;
	my $function = $functions{$name};
	my @parts = parameter_parts($function);
	my @params = parameter_names($function);
	my $definition_ok = $function && @parts == $expected_count &&
		text_of($parts[-1]) =~ /\bu8\b/ && ($params[-1] // '') ne '';
	fail_contract($definition_message) unless $definition_ok;
	my $declaration = $declarations{header}{$name};
	my @declaration_parts = parameter_parts($declaration);
	my $declaration_ok = $declaration && @declaration_parts == $expected_count &&
		text_of($declaration_parts[-1]) =~ /\bu8\b/;
	fail_contract($declaration_message) unless $declaration_ok;
	if ($definition_ok) {
		my $engine = $params[-1];
		my $body = text_of($function->{body});
		fail_contract($command_message)
			unless $body =~ /\breq\s*\.\s*hdr\s*\.\s*band_idx\s*=\s*\Q$engine\E\s*;/;
	}
}

check_sniffer_function(
	'mt7925_mcu_set_sniffer',
	'sniffer enable definition does not take an explicit firmware engine',
	'sniffer enable declaration does not take an explicit firmware engine',
	'sniffer enable command does not use its explicit firmware engine', 4,
);
check_sniffer_function(
	'mt7925_mcu_config_sniffer',
	'sniffer configuration definition does not take an explicit firmware engine',
	'sniffer configuration declaration does not take an explicit firmware engine',
	'sniffer configuration command does not use its explicit firmware engine', 3,
);
my $sniffer_text = join ' ', map {
	exists $functions{$_} ? text_of($functions{$_}{body}) : ''
} qw(mt7925_mcu_set_sniffer mt7925_mcu_config_sniffer);
fail_contract('sniffer command touches mutable global chandef state')
	if $sniffer_text =~ /\bchandef\b/;

fail_contract('engine mapping is not exactly 2.4 GHz to 0 and 5/6 GHz to 1')
	unless direct_switch_map($functions{mt7927_band_to_engine}) ||
		direct_if_map($functions{mt7927_band_to_engine});

my $transaction = $functions{mt7927_reconfig_band};
my @transaction_params = parameter_names($transaction);
my $transaction_dev = $transaction_params[0] // '';
my $same_if = top_if($transaction, 'mt7927_same_engine');
my @same_roots = $same_if ? map { $_->{name} }
	grep { exists $functions{$_->{name}} } calls_in($same_if->{body}) : ();
my @same_closure = closure(@same_roots);
my %same_calls;
if ($same_if) {
	$same_calls{$_->{name}}++ for calls_in($same_if->{body});
}
for my $name (@same_closure) {
	$same_calls{$_->{name}}++ for calls_in($functions{$name}{body});
}
fail_contract('same-engine transition does not re-arm the sniffer')
	unless $same_calls{mt7925_mcu_set_sniffer};
fail_contract('same-engine transition does not configure the sniffer')
	unless $same_calls{mt7925_mcu_config_sniffer};
my $same_migrates = grep {
	/^(?:mt7927_(?:old|new)_(?:dev|bss)_|mt76_connac_mcu_uni_add_(?:dev|bss))/
} keys %same_calls;
fail_contract('same-engine transition reaches DEV/BSS migration') if $same_migrates;

my @forward = (
	['old_sniffer_off', 'mt7927_old_sniffer_off', 'MT7927_JOURNAL_OLD_SNIFFER_OFF'],
	['old_bss_off', 'mt7927_old_bss_off', 'MT7927_JOURNAL_OLD_BSS_OFF'],
	['old_dev_off', 'mt7927_old_dev_off', 'MT7927_JOURNAL_OLD_DEV_OFF'],
	['new_dev_on', 'mt7927_new_dev_on', 'MT7927_JOURNAL_NEW_DEV_ON'],
	['new_bss_on', 'mt7927_new_bss_on', 'MT7927_JOURNAL_NEW_BSS_ON'],
	['new_sniffer_on', 'mt7927_new_sniffer_on', 'MT7927_JOURNAL_NEW_SNIFFER_ON'],
	['new_sniffer_config', 'mt7927_new_sniffer_config', 'MT7927_JOURNAL_NEW_SNIFFER_CONFIG'],
	['publish_band', 'mt7927_publish_band', 'MT7927_JOURNAL_HOST_PUBLISHED'],
);
my $transaction_text = $transaction ? text_of($transaction->{body}) : '';
my %forward_journal_candidates;
while ($transaction_text =~ /\b(\w+)\s*\|=\s*BIT\s*\(/g) {
	$forward_journal_candidates{$1}++;
}
my %journal_declarations;
while ($transaction_text =~ /\b(?:unsigned\s+long|u64|u32|u16|u8|int)\s+
	(\w+)\s*=\s*([^;]+)\s*;/gx) {
	$journal_declarations{$1} = $2;
}
my @journal_candidates = grep {
	exists $forward_journal_candidates{$_} &&
	exists $journal_declarations{$_}
} keys %journal_declarations;
my $journal_var = @journal_candidates == 1 ? $journal_candidates[0] : undef;
my $journal_ok = defined($journal_var) &&
	$journal_declarations{$journal_var} =~ /^\s*0\s*$/;
my (@forward_positions, %forward_error_vars);
for my $stage (@forward) {
	my ($label, $helper, $bit) = @$stage;
	my $pattern = qr/\b(\w+)\s*=\s*\Q$helper\E\s*\([^;]*\)\s*;
		\s*if\s*\(\s*\1\s*<\s*0\s*\)\s*goto\s+rollback\s*;
		\s*(\w+)\s*\|=\s*BIT\s*\(\s*\Q$bit\E\s*\)\s*;/x;
	my $bit_count = () = $transaction_text =~ /\b\Q$bit\E\b/g;
	my $forward_stage_ok = $transaction_text =~ /$pattern/;
	$forward_error_vars{$1}++ if $forward_stage_ok;
	fail_contract("forward stage $label is not success-checked before journaling")
		unless $forward_stage_ok && $bit_count == 1;
	my ($stage_journal_var) = $transaction_text =~ /\b(\w+)\s*\|=\s*BIT\s*\(\s*\Q$bit\E\s*\)/;
	$journal_ok = 0 if defined($stage_journal_var) &&
		(!defined($journal_var) || $stage_journal_var ne $journal_var);
	push @forward_positions, index($transaction_text, "$helper (");
}
my @forward_error_candidates = keys %forward_error_vars;
my $transaction_error = @forward_error_candidates == 1
	? $forward_error_candidates[0] : undef;
my $forward_order = 1;
for (my $i = 0; $i < @forward_positions; $i++) {
	$forward_order = 0 if $forward_positions[$i] < 0 ||
		($i && $forward_positions[$i] <= $forward_positions[$i - 1]);
}
fail_contract('cross-engine forward stages are not in required order')
	unless $forward_order;
my @disable_calls = calls_named($transaction, 'mt76_worker_disable');
my @drain_calls = calls_named($transaction, 'wait_event_timeout');
my @first_forward_calls = calls_named($transaction, $forward[0][1]);
my $tx_worker_arg = "& $transaction_dev -> mt76 . tx_worker";
my $tx_wait_arg = "$transaction_dev -> mt76 . tx_wait";
my $tx_pending_arg = "! mt76_has_tx_pending ( & $transaction_dev -> mphy )";
my $tx_quiesce_ok = $transaction_dev ne '' && @disable_calls == 1 &&
	@{$disable_calls[0]{args}} == 1 &&
	text_of($disable_calls[0]{args}[0]) eq $tx_worker_arg &&
	@drain_calls == 1 && @{$drain_calls[0]{args}} == 3 &&
	text_of($drain_calls[0]{args}[0]) eq $tx_wait_arg &&
	text_of($drain_calls[0]{args}[1]) eq $tx_pending_arg &&
	@{$drain_calls[0]{args}[2]} > 0 && @first_forward_calls == 1 &&
	$disable_calls[0]{position} < $drain_calls[0]{position} &&
	$drain_calls[0]{position} < $first_forward_calls[0]{position};
fail_contract('cross-engine transaction does not quiesce and drain TX')
	unless $tx_quiesce_ok;
my ($forward_success) = $transaction_text =~ /
	\b\Q$forward[-1][2]\E\b\s*\)\s*;\s*(.*?)\brollback\s*:
/x;
$forward_success //= '';
my $tx_parked_param = @transaction_params >= 7 ? $transaction_params[6] : '';
my $resume_function = $functions{mt7927_resume_tx};
my $resume_text = $resume_function ? text_of($resume_function->{body}) : '';
my $resume_helper_ok = $transaction_dev ne '' &&
	$resume_text =~ /\bmt76_worker_enable\s*\(\s*\Q$tx_worker_arg\E\s*\)\s*;/ &&
	$resume_text =~ /\bmt76_worker_schedule\s*\(\s*\Q$tx_worker_arg\E\s*\)\s*;/ &&
	$resume_text =~ /\bieee80211_wake_queues\s*\(\s*\Q$transaction_dev\E\s*->\s*mphy\s*\.\s*hw\s*\)\s*;/;
my $forward_parked_resume = $tx_parked_param ne '' && $resume_helper_ok &&
	$forward_success =~ /\bif\s*\(\s*!\s*\Q$tx_parked_param\E\s*\)\s*
	\bmt7927_resume_tx\s*\(\s*\Q$transaction_dev\E\s*\)\s*;/x;
fail_contract('forward success does not resume TX')
	unless $transaction_dev ne '' &&
		($forward_success =~ /\bmt76_worker_enable\s*\(\s*\Q$tx_worker_arg\E\s*\)\s*;/ ||
		 $forward_parked_resume);
fail_contract('forward success schedules reset')
	if $forward_success =~
		/\b(?:queue_work|schedule_work)\s*\([^;]*\breset_work\b[^;]*\)\s*;/;

my @rollback = (
	['restore_band', 'mt7927_restore_band', 'MT7927_JOURNAL_HOST_PUBLISHED'],
	['new_sniffer_unconfig', 'mt7927_new_sniffer_unconfig', 'MT7927_JOURNAL_NEW_SNIFFER_CONFIG'],
	['new_sniffer_off', 'mt7927_new_sniffer_off', 'MT7927_JOURNAL_NEW_SNIFFER_ON'],
	['new_bss_off', 'mt7927_new_bss_off', 'MT7927_JOURNAL_NEW_BSS_ON'],
	['new_dev_off', 'mt7927_new_dev_off', 'MT7927_JOURNAL_NEW_DEV_ON'],
	['old_dev_on', 'mt7927_old_dev_on', 'MT7927_JOURNAL_OLD_DEV_OFF'],
	['old_bss_on', 'mt7927_old_bss_on', 'MT7927_JOURNAL_OLD_BSS_OFF'],
	['old_sniffer_restore', 'mt7927_old_sniffer_restore', 'MT7927_JOURNAL_OLD_SNIFFER_OFF'],
);
my $rollback_function = $functions{mt7927_reconfig_rollback};
my $rollback_text = $rollback_function ? text_of($rollback_function->{body}) : '';
my @rollback_positions;
my @rollback_params = parameter_names($rollback_function);
my $rollback_journal_param = @rollback_params >= 4 ? $rollback_params[3] : undef;
my @rollback_calls = calls_named($transaction, 'mt7927_reconfig_rollback');
$journal_ok = 0 unless defined($journal_var) &&
	defined($rollback_journal_param) && @rollback_calls == 1 &&
	@{$rollback_calls[0]{args}} >= 4 &&
	text_of($rollback_calls[0]{args}[3]) eq $journal_var;
for my $stage (@rollback) {
	my ($label, $helper, $bit) = @$stage;
	my $pattern = qr/\bif\s*\(\s*(\w+)\s*&\s*BIT\s*\(\s*\Q$bit\E\s*\)\s*\)
		\s*\{\s*(\w+)\s*=\s*\Q$helper\E\s*\([^;]*\)\s*;
		\s*if\s*\(\s*\2\s*<\s*0\s*\)\s*return\s+\2\s*;\s*\}/x;
	fail_contract("rollback stage $label is not guarded and failure-checked")
		unless $rollback_text =~ /$pattern/;
	my ($guard_journal_var) = $rollback_text =~ /\bif\s*\(\s*(\w+)\s*&\s*BIT\s*\(\s*\Q$bit\E\s*\)/;
	$journal_ok = 0 if defined($guard_journal_var) &&
		(!defined($rollback_journal_param) ||
		 $guard_journal_var ne $rollback_journal_param);
	push @rollback_positions, index($rollback_text, "$helper (");
}
my $rollback_order = 1;
for (my $i = 0; $i < @rollback_positions; $i++) {
	$rollback_order = 0 if $rollback_positions[$i] < 0 ||
		($i && $rollback_positions[$i] <= $rollback_positions[$i - 1]);
}
fail_contract('cross-engine rollback stages are not in exact reverse order')
	unless $rollback_order;
fail_contract('cross-engine journal is not initialized and consistently threaded')
	unless $journal_ok;

my ($rollback_tail) = $transaction_text =~ /\brollback\s*:\s*(.*)\z/;
$rollback_tail //= '';
my ($primary, $forward_error) = $rollback_tail =~ /\A\s*(\w+)\s*=\s*(\w+)\s*;/;
my ($rollback_error) = $rollback_tail =~ /\b(\w+)\s*=\s*mt7927_reconfig_rollback\s*\(/;
my $rollback_call_position = defined($rollback_error)
	? index($rollback_tail, "$rollback_error = mt7927_reconfig_rollback (") : -1;
my $before_rollback_call = $rollback_call_position >= 0
	? substr($rollback_tail, 0, $rollback_call_position) : '';
my $after_rollback_call = $rollback_call_position >= 0
	? substr($rollback_tail, $rollback_call_position) : '';
my $primary_writes_before = defined($primary)
	? writes_to_var($before_rollback_call, $primary) : 0;
my $primary_writes_after = defined($primary)
	? writes_to_var($after_rollback_call, $primary) : 0;
my $primary_ok = defined($primary) && defined($forward_error) &&
	defined($rollback_error) && defined($transaction_error) &&
	$forward_error eq $transaction_error && $forward_error ne $rollback_error &&
	$primary_writes_before == 1 && $primary_writes_after == 0 &&
	index($rollback_tail, "$primary = $forward_error ;") <
	$rollback_call_position;
fail_contract('rollback path does not preserve the primary forward error')
	unless $primary_ok;

my $rollback_failure_if = $transaction && $rollback_error
	? top_if({ body => tokenize($rollback_tail), params => [] }, undef) : undef;
my $rollback_failure_text = $rollback_failure_if
	? text_of($rollback_failure_if->{body}) : '';
my $rollback_failure_condition = $rollback_failure_if
	? text_of($rollback_failure_if->{condition}) : '';
my $rollback_failure_condition_ok = $rollback_error &&
	($rollback_failure_condition eq "$rollback_error < 0" ||
	 $rollback_failure_condition eq "0 > $rollback_error");
my $reset_count = () = $rollback_tail =~ /\b(?:queue_work|schedule_work)\s*\([^;]*\breset_work\b[^;]*\)\s*;/g;
my @failure_calls = $rollback_failure_if
	? top_level_calls_in($rollback_failure_if->{body}) : ();
my @failure_resets = grep {
	($_->{name} eq 'queue_work' || $_->{name} eq 'schedule_work') &&
	grep { text_of($_) =~ /\breset_work\b/ } @{$_->{args}}
} @failure_calls;
my @failure_returns = $rollback_failure_if
	? top_level_returns_in($rollback_failure_if->{body}) : ();
my $failure_order_ok = !has_top_level_control($rollback_failure_if->{body}) &&
	@failure_resets == 1 && @failure_returns == 1 &&
	$failure_resets[0]{position} < $failure_returns[0]{position};
fail_contract('rollback failure does not schedule reset exclusively')
	unless $rollback_failure_if && $rollback_failure_condition_ok &&
		$reset_count == 1 && $failure_order_ok;
fail_contract('rollback failure resumes TX')
	if $rollback_failure_text =~ /\b(?:mt76_worker_enable|mt7927_resume_tx)\s*\(/;

my $after_failure = '';
my $after_failure_tokens = [];
if ($rollback_failure_if) {
	my $tail_tokens = tokenize($rollback_tail);
	$after_failure_tokens = slice($tail_tokens, $rollback_failure_if->{after},
		@$tail_tokens - 1);
	$after_failure = text_of($after_failure_tokens);
}
my @success_calls = top_level_calls_in($after_failure_tokens);
my @success_resumes = grep {
	$_->{name} eq 'mt76_worker_enable' && @{$_->{args}} == 1 &&
	text_of($_->{args}[0]) eq $tx_worker_arg
} @success_calls;
my @success_returns = top_level_returns_in($after_failure_tokens);
my $success_order_ok = !has_top_level_control($after_failure_tokens) &&
	@success_resumes == 1 && @success_returns == 1 &&
	$success_resumes[0]{position} < $success_returns[0]{position};
my $rollback_parked_resume = $tx_parked_param ne '' && $resume_helper_ok &&
	$after_failure =~ /\A\s*if\s*\(\s*!\s*\Q$tx_parked_param\E\s*\)\s*
	\bmt7927_resume_tx\s*\(\s*\Q$transaction_dev\E\s*\)\s*;
	\s*return\s+\Q$primary\E\s*;\s*\z/x;
fail_contract('successful rollback does not resume TX')
	unless $transaction_dev ne '' && ($success_order_ok || $rollback_parked_resume);
fail_contract('successful rollback does not return the primary forward error')
	if $primary_ok && (!(grep { $_->{value} eq $primary } @success_returns) ||
		!(grep { $_->{value} eq $primary } @failure_returns));

sub exact_link_connection
{
	my ($function, $missing_return) = @_;
	return 0 unless $function;
	my $body = text_of($function->{body});
	return 0 unless $body =~ /\b(\w+)\s*=\s*mt792x_vif_to_link_exact\s*\(
		[^,]+,\s*(\w+)\s*->\s*link_id\s*\)\s*;/x;
	my $mconf = $1;
	my $missing = $missing_return eq ''
		? qr/\bif\s*\(\s*!\s*\Q$mconf\E\s*\)\s*return\s*;/
		: qr/\bif\s*\(\s*!\s*\Q$mconf\E\s*\)\s*return\s+\Q$missing_return\E\s*;/;
	return 0 unless $body =~ /$missing/;
	return 0 unless $body =~ /\b(\w+)\s*=\s*\Q$mconf\E\s*->\s*mt76\s*\.\s*wcid\s*;/;
	my $wcid = $1;
	for my $call (calls_named($function, 'mt7927_reconfig_band')) {
		next unless @{$call->{args}} >= 3;
		return 1 if text_of($call->{args}[1]) eq $mconf &&
			text_of($call->{args}[2]) eq $wcid;
	}
	return 0;
}

fail_contract('MLO assignment does not connect exact link and WCID to migration')
	unless exact_link_connection($functions{mt7925_assign_vif_chanctx}, '- ENOENT');
fail_contract('MLO unassignment does not connect exact link and WCID to migration')
	unless exact_link_connection($functions{mt7925_unassign_vif_chanctx}, '');

my $identity_ok = 1;
if (@transaction_params < 3) {
	$identity_ok = 0;
} else {
	for my $stage (@forward) {
		my @calls = calls_named($transaction, $stage->[1]);
		$identity_ok = 0 unless @calls == 1 && @{$calls[0]{args}} >= 3 &&
			text_of($calls[0]{args}[1]) eq $transaction_params[1] &&
			text_of($calls[0]{args}[2]) eq $transaction_params[2];
	}
	my @calls = calls_named($transaction, 'mt7927_reconfig_rollback');
	$identity_ok = 0 unless @calls == 1 && @{$calls[0]{args}} >= 3 &&
		text_of($calls[0]{args}[1]) eq $transaction_params[1] &&
		text_of($calls[0]{args}[2]) eq $transaction_params[2];
}
if (@rollback_params < 3) {
	$identity_ok = 0;
} else {
	for my $stage (@rollback) {
		my @calls = calls_named($rollback_function, $stage->[1]);
		$identity_ok = 0 unless @calls == 1 && @{$calls[0]{args}} >= 3 &&
			text_of($calls[0]{args}[1]) eq $rollback_params[1] &&
			text_of($calls[0]{args}[2]) eq $rollback_params[2];
	}
}
fail_contract('migration stages do not preserve exact mconf and WCID')
	unless $identity_ok;

my @surface = closure(qw(
	mt7925_monitor_update_chan mt7925_monitor_sync mt7927_reconfig_band
	mt7925_assign_vif_chanctx mt7925_unassign_vif_chanctx
	mt7925_switch_vif_chanctx
));
my $surface_text = join ' ', map { text_of($functions{$_}{body}) } @surface;
fail_contract('monitor lifecycle helper closure writes ordinary basic_rates_idx')
	if $surface_text =~ /(?:->|\.)\s*basic_rates_idx\s*=/;
fail_contract('monitor lifecycle helper closure writes ordinary wcid phy_idx')
	if $surface_text =~ /\bwcid\s*(?:->|\.)\s*phy_idx\s*=/;
my $chandef_surface = join ' ', map {
	($_ eq 'mt7925_mcu_set_sniffer' || $_ eq 'mt7925_mcu_config_sniffer')
		? '' : text_of($functions{$_}{body})
} @surface;
fail_contract('monitor lifecycle helper closure writes global PHY chandef')
	if $chandef_surface =~ /(?:->|\.)\s*chandef\s*=/;

my $legacy = $functions{mt76_connac_mcu_uni_add_dev};
my $enable_if = top_if($legacy, undef);
my $enable_text = $enable_if ? text_of($enable_if->{body}) : '';
my $enable_ok = $enable_if &&
	text_of($enable_if->{condition}) eq 'enable' &&
	$enable_text =~ /\A(\w+)\s*=\s*
	mt76_connac_mcu_uni_add_dev_info\s*\([^;]*,\s*true\s*\)\s*;
	\s*if\s*\(\s*\1\s*<\s*0\s*\)\s*return\s+\1\s*;
	\s*return\s+mt76_connac_mcu_uni_add_bss_info\s*\([^;]*,\s*true\s*\)\s*;\z/x;
fail_contract('legacy enable branch is not exclusively DEV-on then checked BSS-on')
	unless $enable_ok;
my $disable_text = '';
if ($legacy && $enable_if) {
	$disable_text = text_of(slice($legacy->{body}, $enable_if->{after},
		@{$legacy->{body}} - 1));
}
my $disable_ok = $disable_text =~ /\A(\w+)\s*=\s*
	mt76_connac_mcu_uni_add_bss_info\s*\([^;]*,\s*false\s*\)\s*;
	\s*if\s*\(\s*\1\s*<\s*0\s*\)\s*return\s+\1\s*;
	\s*return\s+mt76_connac_mcu_uni_add_dev_info\s*\([^;]*,\s*false\s*\)\s*;\z/x;
fail_contract('legacy disable branch is not exclusively BSS-off then DEV-off')
	unless $disable_ok;

my $switch = $functions{mt7925_switch_vif_chanctx};
my $switch_text = $switch ? text_of($switch->{body}) : '';
my $switch_validation = $switch_text =~ /\bif\s*\(\s*!\s*(\w+)\s*\|\|\s*(\w+)\s*<=\s*0\s*\)
	\s*return\s+-\s*EINVAL\s*;/x &&
	$switch_text =~ /\bswitch\s*\(\s*\w+\s*\)/ &&
	$switch_text =~ /\bcase\s+CHANCTX_SWMODE_REASSIGN_VIF\s*:/ &&
	$switch_text =~ /\bcase\s+CHANCTX_SWMODE_SWAP_CONTEXTS\s*:/;
fail_contract('chanctx switch does not validate batch and mode')
	unless $switch_validation;
my (@loops, @loop_variables);
if ($switch) {
	my $items = $switch->{body};
	for (my $i = 0; $i + 1 < @$items; $i++) {
		next unless $items->[$i] eq 'for' && $items->[$i + 1] eq '(';
		my $end = matching($items, $i + 1, '(', ')');
		next unless defined $end;
		my $condition = text_of(slice($items, $i + 2, $end - 1));
		my ($index) = $condition =~ /\b(\w+)\s*=\s*0\s*;\s*\1\s*</;
		my ($first, $last) = statement_range($items, $end + 1);
		push @loops, text_of(slice($items, $first, $last));
		push @loop_variables, $index // '';
	}
}
my $first_index = @loop_variables ? quotemeta($loop_variables[0]) : '';
my $second_index = @loop_variables > 1 ? quotemeta($loop_variables[1]) : '';
my $switch_migration = @loops == 2 && $first_index ne '' && $second_index ne '' &&
	$loops[0] =~ /\bmt7927_reconfig_band\s*\([^,]+,\s*
		\w+\s*\[\s*$first_index\s*\]\s*\.\s*mconf\s*,/x &&
	$loops[0] !~ /->\s*mt76\s*\.\s*ctx\s*=/ &&
	$loops[1] =~ /\w+\s*\[\s*$second_index\s*\]\s*\.\s*mconf\s*
		->\s*mt76\s*\.\s*ctx\s*=/x;
fail_contract('chanctx switch does not migrate every member before publication')
	unless $switch_migration;
my $lock_count = () = $switch_text =~ /\bmutex_lock\s*\(/g;
my $unlock_count = () = $switch_text =~ /\bmutex_unlock\s*\(/g;
my $lock_pos = index($switch_text, 'mutex_lock (');
my $migration_pos = index($switch_text, 'mt7927_reconfig_band (');
my $publish_pos = index($switch_text, '-> mt76 . ctx =');
my $unlock_pos = index($switch_text, 'mutex_unlock (');
fail_contract('chanctx switch does not hold one mutex across migration and publication')
	if $switch_migration && !($lock_count == 1 && $unlock_count == 1 &&
		$lock_pos >= 0 && $migration_pos > $lock_pos &&
		$publish_pos > $migration_pos && $unlock_pos > $publish_pos);

if (@failures) {
	print STDERR "FAIL: $_\n" for @failures;
	printf STDERR "FAIL: MT7927 driver lifecycle contract (%d violations)\n",
		scalar @failures;
	exit 1;
}

print "PASS: MT7927 driver lifecycle transaction contract\n";
PERL
