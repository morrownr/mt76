# SPDX-License-Identifier: BSD-3-Clause-Clear

obj-m += mt7925_common_git.o

ifneq ($(CONFIG_PCI),)
obj-m += mt7925e_git.o
endif

obj-m += mt7925u_git.o

mt7925_common_git-y := mac.o mcu.o regd.o main.o init.o debugfs.o
mt7925_common_git-$(CONFIG_NL80211_TESTMODE) += testmode.o
mt7925e_git-y := pci.o pci_mac.o pci_mcu.o
mt7925u_git-y := usb.o
