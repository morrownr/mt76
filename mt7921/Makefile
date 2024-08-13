# SPDX-License-Identifier: ISC

obj-m += mtk7921-common.o
# obj-$(CONFIG_MT7921E) += mt7921e.o
# obj-$(CONFIG_MT7921S) += mt7921s.o
obj-m += mtk7921u.o

mtk7921-common-y := mac.o mcu.o main.o init.o debugfs.o
# mt7921-common-$(CONFIG_NL80211_TESTMODE) += testmode.o
# mt7921e-y := pci.o pci_mac.o pci_mcu.o
# mt7921s-y := sdio.o sdio_mac.o sdio_mcu.o
mtk7921u-y := usb.o
