#!/bin/sh
set -eu

repo="${1:-.}"
file="$repo/mt792x_mac.c"

awk '
	/void mt792x_mac_work\(struct work_struct \*work\)/ {
		in_fn = 1
	}

	in_fn && /test_bit\(MT76_REMOVED, &mphy->state\)/ {
		saw_removed = 1
	}

	in_fn && /test_bit\(MT76_STATE_RUNNING, &mphy->state\)/ {
		saw_running = 1
	}

	in_fn && /ieee80211_queue_delayed_work\(phy->mt76->hw, &mphy->mac_work,/ {
		if (!saw_removed || !saw_running) {
			print "FAIL: mac_work requeues without removed/running guards"
			exit 1
		}
		found_queue = 1
	}

	in_fn && /EXPORT_SYMBOL_GPL\(mt792x_mac_work\);/ {
		if (!found_queue) {
			print "FAIL: mac_work requeue contract was not checked"
			exit 1
		}
		found = 1
		exit 0
	}

	END {
		if (!found) {
			print "FAIL: mt792x_mac_work contract did not complete"
			exit 1
		}
	}
' "$file"

awk '
	/void mt792x_pm_wake_work\(struct work_struct \*work\)/ {
		in_fn = 1
	}

	in_fn && /test_bit\(MT76_REMOVED, &mphy->state\)/ {
		saw_removed = 1
	}

	in_fn && /test_bit\(MT76_STATE_RUNNING, &mphy->state\)/ {
		saw_running = 1
	}

	in_fn && /ieee80211_queue_delayed_work\(mphy->hw, &mphy->mac_work,/ {
		if (!saw_removed || !saw_running) {
			print "FAIL: wake_work requeues mac_work without removed/running guards"
			exit 1
		}
		found_queue = 1
	}

	in_fn && /EXPORT_SYMBOL_GPL\(mt792x_pm_wake_work\);/ {
		if (!found_queue) {
			print "FAIL: wake_work mac_work requeue contract was not checked"
			exit 1
		}
		found = 1
		exit 0
	}

	END {
		if (!found) {
			print "FAIL: mt792x_pm_wake_work contract did not complete"
			exit 1
		}
	}
' "$file"

echo "PASS mac_work and wake_work requeue only while running and not removed"
