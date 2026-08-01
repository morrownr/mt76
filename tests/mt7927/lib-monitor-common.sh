#!/bin/sh

monitor_die()
{
	printf 'monitor-contract: %s\n' "$*" >&2
	exit 2
}

monitor_validate_uint()
{
	case $2 in
		''|*[!0-9]*|0) monitor_die "$1 must be a positive integer" ;;
	esac
}

monitor_validate_iface()
{
	case $1 in
		''|*[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-]*)
			monitor_die 'interface contains unsupported characters'
			;;
	esac
}

monitor_validate_band()
{
	case $1 in
		2.4|5|6) ;;
		*) monitor_die "unsupported band: $1" ;;
	esac
}

monitor_validate_frequency()
{
	band=$1
	freq=$2
	case $freq in ''|*[!0-9]*) monitor_die 'frequency must be an integer MHz value' ;; esac
	case $band in
		2.4) if [ "$freq" -lt 2412 ] || [ "$freq" -gt 2484 ]; then monitor_die 'frequency is outside 2.4 GHz'; fi ;;
		5) if [ "$freq" -lt 5000 ] || [ "$freq" -gt 5895 ]; then monitor_die 'frequency is outside 5 GHz'; fi ;;
		6) if [ "$freq" -lt 5955 ] || [ "$freq" -gt 7115 ]; then monitor_die 'frequency is outside 6 GHz'; fi ;;
	esac
}

monitor_default_frequency()
{
	case $1 in
		2.4) printf '%s\n' 2412 ;;
		5) printf '%s\n' 5180 ;;
		6) printf '%s\n' 5955 ;;
	esac
}

monitor_enabled_frequencies()
{
	awk '
		/\* [0-9]+([.][0-9]+)? MHz/ && $0 !~ /\(disabled\)/ {
			for (i = 1; i <= NF; i++)
				if ($i == "MHz") {
					freq = $(i - 1)
					sub(/[.]0$/, "", freq)
					printf "%s%s", sep, freq
					sep = ","
				}
		}
		END { if (sep != "") print "" }
	' "$1"
}

monitor_band_for_frequency()
{
	case $1 in
		24[0-9][0-9]) printf '%s\n' 2.4 ;;
		5[0-8][0-9][0-9]) printf '%s\n' 5 ;;
		59[0-9][0-9]|6[0-9][0-9][0-9]|7[01][0-9][0-9]) printf '%s\n' 6 ;;
		*) monitor_die "unsupported frequency: $1" ;;
	esac
}

monitor_validate_bands()
{
	old_ifs=$IFS
	IFS=,
	set -f
	# Intentional comma-delimited field split after globbing is disabled.
	# shellcheck disable=SC2086
	set -- $1
	set +f
	IFS=$old_ifs
	[ "$#" -gt 0 ] || monitor_die 'transitions must not be empty'
	for band do monitor_validate_band "$band"; done
}

monitor_run()
{
	printf 'command='
	printf ' %s' "$@"
	printf '\n'
	[ "$MONITOR_MODE" = execute ] || return 0
	"$@"
}

monitor_prepare_interface()
{
	if [ "$MONITOR_MODE" = execute ] &&
	    iw dev "$MONITOR_IFACE" info >/dev/null 2>&1; then
		monitor_run iw dev "$MONITOR_IFACE" del
	fi
	monitor_run iw phy "$MONITOR_PHY" interface add "$MONITOR_IFACE" type monitor
}

monitor_set_frequency()
{
	band=$1
	freq=$(monitor_default_frequency "$band")
	if [ -n "$MONITOR_SINGLE_BAND" ] && [ "$band" = "$MONITOR_SINGLE_BAND" ]; then
		freq=$MONITOR_SINGLE_FREQ
	fi
	printf 'step=channel band=%s frequency=%s\n' "$band" "$freq"
	monitor_run iw dev "$MONITOR_IFACE" set freq "$freq"
}

monitor_set_frequency_value()
{
	freq=$1
	band=$(monitor_band_for_frequency "$freq")
	printf 'step=channel band=%s frequency=%s\n' "$band" "$freq"
	monitor_run iw dev "$MONITOR_IFACE" set freq "$freq"
}

monitor_plan_round()
{
	round=$1
	printf 'round=%s\n' "$round"
	monitor_run ip link set dev "$MONITOR_IFACE" down
	monitor_run iw dev "$MONITOR_IFACE" set type monitor
	monitor_run ip link set dev "$MONITOR_IFACE" up

	old_ifs=$IFS
	IFS=,
	set -f
	if [ "$MONITOR_ALL_ENABLED" -eq 1 ]; then
		# Intentional comma-delimited field split after globbing is disabled.
		# shellcheck disable=SC2086
		set -- $MONITOR_FREQUENCIES
		set +f
		IFS=$old_ifs
		for freq do monitor_set_frequency_value "$freq"; done
		first_frequency=$1
	else
		# Intentional comma-delimited field split after globbing is disabled.
		# shellcheck disable=SC2086
		set -- $MONITOR_BANDS
		set +f
		IFS=$old_ifs
		for band do monitor_set_frequency "$band"; done
		first_band=$1
	fi

	printf 'transition=chanctx-readd\n'
	monitor_run ip link set dev "$MONITOR_IFACE" down
	monitor_run ip link set dev "$MONITOR_IFACE" up
	if [ "$MONITOR_ALL_ENABLED" -eq 1 ]; then
		monitor_set_frequency_value "$first_frequency"
	else
		monitor_set_frequency "$first_band"
	fi

	printf 'transition=interface-recreate\n'
	monitor_run iw dev "$MONITOR_IFACE" del
	monitor_run iw phy "$MONITOR_PHY" interface add "$MONITOR_IFACE" type monitor
	monitor_run ip link set dev "$MONITOR_IFACE" up
	if [ "$MONITOR_ALL_ENABLED" -eq 1 ]; then
		monitor_set_frequency_value "$first_frequency"
	else
		monitor_set_frequency "$first_band"
	fi
}
