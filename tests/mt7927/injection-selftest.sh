#!/bin/sh
set -eu

TEST_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
contract=$TEST_DIR/injection-contract.sh
head=ca013156cfc2e7641e273a5ea0f6ece896facd43
# shellcheck source=tests/mt7927/lib-injection-contract.sh
. "$TEST_DIR/lib-injection-contract.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
pass=0

ok() { pass=$((pass + 1)); }
fail() { printf 'not ok: %s\n' "$1" >&2; exit 1; }
assert_eq() { [ "$1" = "$2" ] || fail "$3"; ok; }
assert_fails() { message=$1; shift; "$@" >/dev/null 2>&1 && fail "$message unexpectedly succeeded"; ok; }
sha() { sha256sum "$1" | awk '{ print $1 }'; }
row() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >>"$meta"; }
contract_run() { "$contract" "$1" "$2" "$3" "$4" "$5" "$6" "$7"; }

replace()
{
	awk -F '\t' -v OFS='\t' -v kind="$2" -v id="$3" -v field="$4" -v value="$5" \
		'$1 == kind && $2 == id && $3 == field { $4 = value } { print }' "$1" >"$6"
}

set_artifact_hash()
{
	hash=$(sha "$3")
	replace "$1" artifact "$2" sha256 "$hash" "$4"
}

window()
{
	id=$1 listen=$2 packets=$3
	printf '%s\t1\t2\t1000\t1001\towner-%s\t123\tstart-%s\tready-%s\t%s\t%s\t0\thealthy\treceiver.example\tphy1\tmon1\treceiver-device\treceiver-radio\tcapture --session\t%s\t%s\n' \
		"$id" "$id" "$id" "$id" "$listen" "$packets" "$receiver_pcap" "$(sha "$receiver_pcap")" >>"$window"
}

fixture()
{
	probe=$1 send=$2 receiver=$3 reflection=$4 window=$5 meta=$6
	receiver_pcap=$tmp/receiver.pcap status_pcap=$tmp/status.pcap dmesg=$tmp/dmesg build=$tmp/build
	rconv=$tmp/rconv sconv=$tmp/sconv
	printf 'receiver pcap\n' >"$receiver_pcap"; printf 'status pcap\n' >"$status_pcap"
	printf 'mt7927: clean\n' >"$dmesg"; printf 'head %s\nconfig config-hash\n' "$head" >"$build"
	printf 'receiver converter\n' >"$rconv"; printf 'status converter\n' >"$sconv"
	printf 'tag\ttx_frequency_mhz\tlisten_frequency_mhz\tclass\texpectation\treceiver_session_id\texpected_transmitter_addr\n' >"$probe"
	printf 'tag\tsend_status\tsender_ts\n' >"$send"
	printf 'tag\tfrequency_mhz\treceiver_ts\tsession_id\tobserved_transmitter_addr\tframe_control\tsequence_control\n' >"$receiver"
	printf 'tag\tlocal_state\tlocal_ts\tsession_id\tobserved_transmitter_addr\tframe_control\tsequence_control\n' >"$reflection"
	printf 'receiver_session_id\tstart_ts\tready_ts\tstop_ts\tcollect_ts\towner_token\tpid\tstart_identity\tready_nonce\tverified_listen_frequency\tcapture_packets\tcapture_drops\tcapture_liveness\treceiver_host\treceiver_phy\treceiver_interface\treceiver_device\treceiver_radio\tcapture_command\tpcap_path\tpcap_sha256\n' >"$window"
	: >"$tmp/window-spec"; : >"$tmp/frame"
	sequence=0 first_received_tag='' first_absent_tag='' first_absent_session=''
	for pair in '2412 5180' '5180 2412' '5955 5180'; do
		# shellcheck disable=SC2086
		set -- $pair; tx=$1; other=$2
		for class in unicast broadcast; do
			session=rx-$tx-present-$class; printf '%s\t%s\t19\n' "$session" "$tx" >>"$tmp/window-spec"
			count=0
			while [ "$count" -lt 20 ]; do
				sequence=$((sequence + 1)); count=$((count + 1)); tag=$(injection_tag run-7 pre "$tx" "$other" "$class" "$sequence"); sc=$(printf '%04x' "$sequence")
				printf '%s\t%s\t%s\t%s\tpresent\t%s\t02:00:00:00:00:01\n' "$tag" "$tx" "$tx" "$class" "$session" >>"$probe"
				printf '%s\tSENT\t10\n' "$tag" >>"$send"
				printf 'frame\t%s\tframe_control\t0008\nframe\t%s\tsequence_control\t%s\n' "$tag" "$tag" "$sc" >>"$tmp/frame"
				[ "$class" = unicast ] && local=LOCAL_FAILED || local=LOCAL_REFLECTED
				printf '%s\t%s\t20\tstatus-run-7\t02:00:00:00:00:01\t0008\t%s\n' "$tag" "$local" "$sc" >>"$reflection"
				if [ "$count" -gt 1 ]; then
					printf '%s\t%s\t20\t%s\t02:00:00:00:00:01\t0008\t%s\n' "$tag" "$tx" "$session" "$sc" >>"$receiver"
					[ -n "$first_received_tag" ] || first_received_tag=$tag
				fi
			done
		done
		for class in unicast broadcast; do
			sequence=$((sequence + 1)); session=rx-$tx-absent-$class; printf '%s\t%s\t0\n' "$session" "$other" >>"$tmp/window-spec"
			tag=$(injection_tag run-7 pre "$tx" "$other" "$class" "$sequence"); sc=$(printf '%04x' "$sequence")
			printf '%s\t%s\t%s\t%s\tabsent\t%s\t02:00:00:00:00:01\n' "$tag" "$tx" "$other" "$class" "$session" >>"$probe"
			printf '%s\tSENT\t10\n' "$tag" >>"$send"
			printf 'frame\t%s\tframe_control\t0008\nframe\t%s\tsequence_control\t%s\n' "$tag" "$tag" "$sc" >>"$tmp/frame"
			[ "$class" = unicast ] && local=LOCAL_ACKED || local=LOCAL_REFLECTED
			printf '%s\t%s\t20\tstatus-run-7\t02:00:00:00:00:01\t0008\t%s\n' "$tag" "$local" "$sc" >>"$reflection"
			[ -n "$first_absent_tag" ] || { first_absent_tag=$tag; first_absent_session=$session; }
		done
	done
	printf 'kind\tid\tfield\tvalue\n' >"$meta"
	row run run-7 candidate_head "$head"; row run run-7 converter_verified 1
	for field in kernel_release candidate_kernel_release module_vermagic candidate_module_vermagic loaded_module_path build_module_path loaded_module_sha256 build_module_sha256 loaded_module_signer build_module_signer loaded_module_build_id build_module_build_id module_source build_candidate_head build_config_hash; do
		case $field in kernel_release|candidate_kernel_release) value=7.0.12-test;; module_vermagic|candidate_module_vermagic) value='7.0.12-test SMP';; loaded_module_path|build_module_path) value=/kernel/mt7927.ko;; loaded_module_sha256|build_module_sha256) value=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;; loaded_module_signer|build_module_signer) value=lab-signer;; loaded_module_build_id|build_module_build_id) value='build-7';; module_source|build_candidate_head) value=$head;; build_config_hash) value=config-hash;; esac
		row runtime current "$field" "$value"
	done
	row phy tx legal_frequencies '2412 5180 5955'; row phy rx legal_frequencies '2412 5180 5955'
	row transmitter identity phy phy0; row transmitter identity interface mon0; row transmitter identity device transmitter-device; row transmitter identity radio transmitter-radio; row transmitter identity addr 02:00:00:00:00:01
	for item in "probe_plan:$probe" "send_results:$send" "receiver:$receiver" "reflection:$reflection" "window:$window" "receiver_pcap:$receiver_pcap" "status_pcap:$status_pcap" "dmesg:$dmesg" "build_manifest:$build"; do
		id=${item%%:*}; path=${item#*:}; row artifact "$id" path "$path"; row artifact "$id" sha256 "$(sha "$path")"
	done
	row artifact dmesg bounded_lines 100
	row converter receiver identity receiver-pcap-to-tsv.sh; row converter receiver path "$rconv"; row converter receiver sha256 "$(sha "$rconv")"; row converter receiver normalized_sha256 "$(sha "$receiver")"; row converter receiver interface 'SESSION_ID PROBE_PLAN EXPECTED_TX_INTERFACE'
	row converter reflection identity tx-status-pcap-to-tsv.sh; row converter reflection path "$sconv"; row converter reflection sha256 "$(sha "$sconv")"; row converter reflection normalized_sha256 "$(sha "$reflection")"; row converter reflection interface 'SESSION_ID PROBE_PLAN EXPECTED_TX_INTERFACE'
	while read -r session listen packets; do window "$session" "$listen" "$packets"; done <"$tmp/window-spec"
	cat "$tmp/frame" >>"$meta"
}

fixture "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv"
expected='run_id=run-7
positive_sent=120
positive_received=114
positive_delivery_percent=95
normalized_status=PASS'
assert_eq "$(contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7)" "$expected" 'complete normalized evidence passes at exactly 95 percent'

for input in probe send receiver reflection window meta; do
	awk 'NR == 1 { $0 = "wrong\theader" } { print }' "$tmp/$input.tsv" >"$tmp/bad-$input.tsv"
	case $input in
	probe) assert_fails 'exact probe header required' contract_run "$tmp/bad-$input.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7;;
	send) assert_fails 'exact send header required' contract_run "$tmp/probe.tsv" "$tmp/bad-$input.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7;;
	receiver) assert_fails 'exact receiver header required' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/bad-$input.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7;;
	reflection) assert_fails 'exact reflection header required' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/bad-$input.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7;;
	window) assert_fails 'exact window header required' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/bad-$input.tsv" "$tmp/meta.tsv" run-7;;
	meta) assert_fails 'exact metadata header required' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/bad-$input.tsv" run-7;; esac
done
assert_fails 'empty run ID fails' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" ''
assert_fails 'bad run ID fails' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" bad/run
replace "$tmp/meta.tsv" phy tx legal_frequencies '2412 5180 5900' "$tmp/5900-meta.tsv"
assert_fails '5900 is not a legal center' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/5900-meta.tsv" run-7
for input in probe send receiver reflection window meta; do cp "$tmp/$input.tsv" "$tmp/dup-$input.tsv"; sed -n '2p' "$tmp/$input.tsv" >>"$tmp/dup-$input.tsv"; case $input in probe) assert_fails 'duplicate probe tag' contract_run "$tmp/dup-$input.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7;; send) assert_fails 'duplicate send tag' contract_run "$tmp/probe.tsv" "$tmp/dup-$input.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7;; receiver) assert_fails 'duplicate receiver tag' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/dup-$input.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7;; reflection) assert_fails 'duplicate reflection tag' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/dup-$input.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7;; window) assert_fails 'duplicate window session' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/dup-$input.tsv" "$tmp/meta.tsv" run-7;; meta) assert_fails 'duplicate metadata row' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/dup-$input.tsv" run-7;; esac; done
awk -F '\t' 'NR != 2 { print }' "$tmp/receiver.tsv" >"$tmp/below-95.tsv"; assert_fails 'below 95 percent fails' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/below-95.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7
awk -F '\t' -v OFS='\t' 'NR == 2 { $5 = "00:00:00:00:00:00" } { print }' "$tmp/receiver.tsv" >"$tmp/bad-identity.tsv"; assert_fails 'receiver transmitter identity required' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/bad-identity.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7
awk -F '\t' -v OFS='\t' 'NR == 2 { $6 = "0004" } { print }' "$tmp/receiver.tsv" >"$tmp/bad-frame.tsv"; assert_fails 'receiver frame identity required' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/bad-frame.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7
cp "$tmp/receiver.tsv" "$tmp/leak.tsv"; printf '%s\t5180\t20\t%s\t02:00:00:00:00:01\t0008\tffff\n' "$first_absent_tag" "$first_absent_session" >>"$tmp/leak.tsv"; assert_fails 'negative tag leakage fails' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/leak.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7
awk -F '\t' -v OFS='\t' 'NR == 2 { $2 = "FAILED" } { print }' "$tmp/send.tsv" >"$tmp/send-failed.tsv"; assert_fails 'any send failure fails' contract_run "$tmp/probe.tsv" "$tmp/send-failed.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7
awk -F '\t' -v OFS='\t' 'NR == 2 { $3 = 11 } { print }' "$tmp/window.tsv" >"$tmp/outside-window.tsv"; assert_fails 'sender time must be in ready-stop interval' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/outside-window.tsv" "$tmp/meta.tsv" run-7
awk -F '\t' -v OFS='\t' 'NR == 7 { $13 = "dead" } { print }' "$tmp/window.tsv" >"$tmp/dead-window.tsv"; assert_fails 'negative window must be healthy' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/dead-window.tsv" "$tmp/meta.tsv" run-7
replace "$tmp/meta.tsv" transmitter identity radio receiver-radio "$tmp/same-radio-meta.tsv"; assert_fails 'receiver must be distinct' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/same-radio-meta.tsv" run-7
sed '$d' "$tmp/reflection.tsv" >"$tmp/missing-reflection.tsv"; assert_fails 'every planned tag requires local reflection' contract_run "$tmp/probe.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/missing-reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7
awk -F '\t' -v tag="$first_absent_tag" 'NR == 1 || $1 != tag { print }' "$tmp/probe.tsv" >"$tmp/missing-negative.tsv"; assert_fails 'explicit negative window is mandatory' contract_run "$tmp/missing-negative.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7
awk -F '\t' 'NR == 1 || $2 != 5955' "$tmp/probe.tsv" >"$tmp/no-6ghz.tsv"; assert_fails 'positive coverage requires all bands' contract_run "$tmp/no-6ghz.tsv" "$tmp/send.tsv" "$tmp/receiver.tsv" "$tmp/reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7
bad_tag=MT7927-BAD
awk -F '\t' -v OFS='\t' -v tag="$first_received_tag" -v bad="$bad_tag" '$1 == tag { $1 = bad } { print }' "$tmp/probe.tsv" >"$tmp/malformed-probe.tsv"
awk -F '\t' -v OFS='\t' -v tag="$first_received_tag" -v bad="$bad_tag" '$1 == tag { $1 = bad } { print }' "$tmp/send.tsv" >"$tmp/malformed-send.tsv"
awk -F '\t' -v OFS='\t' -v tag="$first_received_tag" -v bad="$bad_tag" '$1 == tag { $1 = bad } { print }' "$tmp/receiver.tsv" >"$tmp/malformed-receiver.tsv"
awk -F '\t' -v OFS='\t' -v tag="$first_received_tag" -v bad="$bad_tag" '$1 == tag { $1 = bad } { print }' "$tmp/reflection.tsv" >"$tmp/malformed-reflection.tsv"
assert_fails 'malformed tag is consistently rejected across joined inputs' contract_run "$tmp/malformed-probe.tsv" "$tmp/malformed-send.tsv" "$tmp/malformed-receiver.tsv" "$tmp/malformed-reflection.tsv" "$tmp/window.tsv" "$tmp/meta.tsv" run-7

printf '1..%s\n' "$pass"
