# SPDX-License-Identifier: GPL-2.0-only

ifneq ($(KERNELRELEASE),)

obj-m += mtk76.o
obj-m += mtk76-usb.o
# obj-$(CONFIG_MT76_SDIO) += mt76-sdio.o
obj-m += mtk76x02-lib.o
obj-m += mtk76x02-usb.o
obj-m += mtk76-connac-lib.o
obj-m += mtk792x-lib.o
obj-m += mtk792x-usb.o

mtk76-y := \
	mmio.o util.o trace.o dma.o mac80211.o debugfs.o eeprom.o \
	tx.o agg-rx.o mcu.o wed.o

# mt76-$(CONFIG_PCI) += pci.o
# mt76-$(CONFIG_NL80211_TESTMODE) += testmode.o

mtk76-usb-y := usb.o usb_trace.o
# mt76-sdio-y := sdio.o sdio_txrx.o

CFLAGS_trace.o := -I$(src)
CFLAGS_usb_trace.o := -I$(src)
CFLAGS_mt76x02_trace.o := -I$(src)
CFLAGS_mt792x_trace.o := -I$(src)

mtk76x02-lib-y := mt76x02_util.o mt76x02_mac.o mt76x02_mcu.o \
		  mt76x02_eeprom.o mt76x02_phy.o mt76x02_mmio.o \
		  mt76x02_txrx.o mt76x02_trace.o mt76x02_debugfs.o \
		  mt76x02_dfs.o mt76x02_beacon.o

mtk76x02-usb-y := mt76x02_usb_mcu.o mt76x02_usb_core.o

mtk76-connac-lib-y := mt76_connac_mcu.o mt76_connac_mac.o mt76_connac3_mac.o

mtk792x-lib-y := mt792x_core.o mt792x_mac.o mt792x_trace.o \
		 mt792x_debugfs.o mt792x_dma.o
mtk792x-lib-$(CONFIG_ACPI) += mt792x_acpi_sar.o
mtk792x-usb-y := mt792x_usb.o

obj-m += mt76x0/
obj-m += mt76x2/
# obj-$(CONFIG_MT7603E) += mt7603/
# obj-$(CONFIG_MT7615_COMMON) += mt7615/
# obj-$(CONFIG_MT7915E) += mt7915/
obj-m += mt7921/
# obj-$(CONFIG_MT7996E) += mt7996/
# obj-$(CONFIG_MT7925_COMMON) += mt7925/

else

SHELL := /bin/sh
KVER ?= `uname -r`
KSRC ?= /lib/modules/$(KVER)/build
MODDIR ?= /lib/modules/$(KVER)/extra
MODLIST := mtk76x0u mtk76x2u mtk7921u \
	   mtk76x0_common mtk76x2_common mtk7921_common \
	   mtk76x02_usb mtk792x_usb mtk76_usb mtk76x02_lib mtk792x_lib \
	   mtk76_connac_lib mtk76

EXTRA_CFLAGS += -std=gnu11 -Wno-declaration-after-statement

.PHONY: modules clean install uninstall sign

modules:
	$(MAKE) -j`nproc` -C $(KSRC) M=$$PWD modules
	@strip -g *.ko mt76x0/*.ko mt76x2/*.ko mt7921/*.ko

clean:
	$(MAKE) -C $(KSRC) M=$$PWD clean
	@rm -f MOK.*

install:
	@install -Dvm 644 -t $(MODDIR)/mt76 *.ko
	@install -Dvm 644 -t $(MODDIR)/mt76/mt76x0 mt76x0/*.ko
	@install -Dvm 644 -t $(MODDIR)/mt76/mt76x2 mt76x2/*.ko
	@install -Dvm 644 -t $(MODDIR)/mt76/mt7921 mt7921/*.ko
	@install -Dvm 644 -t /etc/modprobe.d blacklist-mt76.conf
	depmod -a $(KVER)
	@echo "The mt76 drivers were installed successfully."

uninstall:
	@for mod in $(MODLIST); do \
		rmmod -s $$mod || true; \
	done
	@rm -rvf $(MODDIR)/mt76
	@rmdir --ignore-fail-on-non-empty $(MODDIR) || true
	depmod -a $(KVER)
	@echo "The mt76 drivers were removed successfully."

sign:
ifeq ($(wildcard MOK.der), )
	@echo "Start Creating a MOK(Machine Owner Key)..."
	@openssl req -new -x509 -newkey rsa:2048 -keyout MOK.priv -outform DER -out MOK.der -nodes -days 36500 -subj "/CN=Custom MOK/"
	@mokutil --import MOK.der
else
	@echo "MOK(Machine Owner Key) creation will be skipped as it exists already."
endif
	@for mod in $(shell find . -name "*.ko"); do \
		$(KSRC)/scripts/sign-file sha256 MOK.priv MOK.der $$mod; \
	done
	@echo "All built modules are signed successfully."

endif
