# SPDX-License-Identifier: GPL-2.0-only

obj-m += mtk76x0u.o
# obj-$(CONFIG_MT76x0E) += mt76x0e.o
obj-m += mtk76x0-common.o

mtk76x0-common-y := init.o main.o eeprom.o phy.o

mtk76x0u-y := usb.o usb_mcu.o
# mt76x0e-y := pci.o pci_mcu.o

# ccflags-y := -DDEBUG
