# SPDX-License-Identifier: BSD-3-Clause-Clear

obj-m += mt7927_common_git.o

ifneq ($(CONFIG_PCI),)
obj-m += mt7927e_git.o
endif

mt7927_common_git-y := mac.o mcu.o regd.o main.o init.o debugfs.o
mt7927_common_git-$(CONFIG_NL80211_TESTMODE) += testmode.o
mt7927e_git-y := pci.o pci_mac.o pci_mcu.o
