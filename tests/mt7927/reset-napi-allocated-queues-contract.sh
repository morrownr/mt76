#!/bin/sh
set -eu

repo="${1:-.}"
file="$repo/mt7925/pci_mac.c"

awk '
	/int mt7925e_mac_reset\(struct mt792x_dev \*dev\)/ {
		in_fn = 1
	}

	in_fn && /mt76_for_each_q_rx\(&dev->mt76, i\)/ {
		saw_allocated_queue_loop = 1
	}

	in_fn && /napi_disable\(&dev->mt76\.napi\[MT_RXQ_/ {
		print "FAIL: reset disables RX NAPI by hard-coded queue id"
		exit 1
	}

	in_fn && /mt7925_tx_token_put\(dev\);/ {
		if (!saw_allocated_queue_loop) {
			print "FAIL: reset does not disable RX NAPI through mt76_for_each_q_rx"
			exit 1
		}

		found = 1
		exit 0
	}

	END {
		if (!found) {
			print "FAIL: reset RX NAPI disable contract was not checked"
			exit 1
		}
	}
' "$file"

echo "PASS reset disables only allocated RX NAPI queues"
