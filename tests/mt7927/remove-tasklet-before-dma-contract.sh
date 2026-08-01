#!/bin/sh
set -eu

repo="${1:-.}"
file="$repo/mt7925/pci.c"

awk '
	/static void mt7925e_unregister_device\(struct mt792x_dev \*dev\)/ { in_unreg = 1 }
	in_unreg && /^}/ { in_unreg = 0 }
	in_unreg && /tasklet_disable\(&dev->mt76\.irq_tasklet\);/ {
		print "FAIL: mt7925e_unregister_device disables irq_tasklet without a matching enable; a disabled-but-scheduled tasklet never clears TASKLET_STATE_SCHED and hangs the tasklet_kill() in mt7925_pci_remove()"
		bad = 1
		exit 1
	}

	/static void mt7925_pci_remove\(struct pci_dev \*pdev\)/ { in_remove = 1 }
	in_remove && /mt7925e_unregister_device\(dev\);/ { unregister_line = NR }
	in_remove && /tasklet_kill\(&dev->mt76\.irq_tasklet\);/ { kill_line = NR }
	in_remove && /mt76_free_device\(&dev->mt76\);/ { free_line = NR }
	in_remove && /^}/ {
		if (!unregister_line) {
			print "FAIL: pci_remove does not call mt7925e_unregister_device"
			exit 1
		}
		if (!kill_line) {
			print "FAIL: pci_remove does not tasklet_kill irq_tasklet"
			exit 1
		}
		if (!free_line) {
			print "FAIL: pci_remove does not call mt76_free_device"
			exit 1
		}
		if (kill_line < unregister_line) {
			print "FAIL: irq_tasklet is killed before unregister_device runs"
			exit 1
		}
		if (kill_line > free_line) {
			print "FAIL: irq_tasklet is killed after mt76_free_device frees dev"
			exit 1
		}
		found = 1
		exit 0
	}
	END {
		if (bad) exit 1
		if (!found) {
			print "FAIL: mt7925_pci_remove contract was not checked"
			exit 1
		}
	}
' "$file"

echo "PASS irq_tasklet is not left disabled in unregister, and is killed between unregister and free in remove"
