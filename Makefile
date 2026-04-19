# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Out-of-tree Makefile for the mt76 MediaTek WiFi driver family
#
# All modules are built with a _git suffix so they coexist alongside
# the in-kernel mt76 drivers. The blacklist in mt76_git.conf controls
# which version loads at runtime.
#
# Based on: https://github.com/openwrt/mt76
# Adapted for standalone out-of-tree building by the morrownr community.
#
# Usage:
#   make                       Build all modules for the running kernel
#   make KVER=6.12.0-generic   Build for a specific kernel version
#   make clean                 Clean build artifacts
#   sudo make install          Install modules and run depmod
#   sudo make install_fw       Install firmware files
#   sudo make uninstall        Remove installed modules

ifneq ($(KERNELRELEASE),)

# ============================================================
# Kbuild section -- processed inside the kernel build system
# ============================================================

# Bake the git commit into every module for traceability (visible via modinfo)
GIT_COMMIT := $(shell git --git-dir=$(src)/.git rev-parse --short HEAD 2>/dev/null || echo "unknown")
EXTRA_CFLAGS += -Werror -DCONFIG_MT76_LEDS -DCONFIG_MT76_DEBUGFS \
	-DGIT_COMMIT=\"$(GIT_COMMIT)\"

# --- Core ---
obj-m += mt76_git.o

mt76_git-y := \
	mmio.o util.o trace.o dma.o mac80211.o debugfs.o eeprom.o \
	tx.o agg-rx.o mcu.o wed.o scan.o channel.o

mt76_git-$(CONFIG_MT76_NPU) += npu.o
mt76_git-$(CONFIG_PCI) += pci.o
mt76_git-$(CONFIG_NL80211_TESTMODE) += testmode.o

# --- USB transport ---
obj-m += mt76_usb_git.o
mt76_usb_git-y := usb.o usb_trace.o

# --- SDIO transport ---
ifneq ($(CONFIG_MMC),)
obj-m += mt76_sdio_git.o
mt76_sdio_git-y := sdio.o sdio_txrx.o
endif

# --- Legacy shared library (mt76x0 / mt76x2) ---
obj-m += mt76x02_lib_git.o
mt76x02_lib_git-y := \
	mt76x02_util.o mt76x02_mac.o mt76x02_mcu.o \
	mt76x02_eeprom.o mt76x02_phy.o mt76x02_mmio.o \
	mt76x02_txrx.o mt76x02_trace.o mt76x02_debugfs.o \
	mt76x02_dfs.o mt76x02_beacon.o

obj-m += mt76x02_usb_git.o
mt76x02_usb_git-y := mt76x02_usb_mcu.o mt76x02_usb_core.o

# --- CONNAC shared library ---
obj-m += mt76_connac_lib_git.o
mt76_connac_lib_git-y := mt76_connac_mcu.o mt76_connac_mac.o mt76_connac3_mac.o

# --- MT792x shared library ---
obj-m += mt792x_lib_git.o
mt792x_lib_git-y := \
	mt792x_core.o mt792x_mac.o mt792x_trace.o \
	mt792x_debugfs.o mt792x_dma.o
mt792x_lib_git-$(CONFIG_ACPI) += mt792x_acpi_sar.o

obj-m += mt792x_usb_git.o
mt792x_usb_git-y := mt792x_usb.o

# --- Trace include paths ---
CFLAGS_trace.o := -I$(src)
CFLAGS_usb_trace.o := -I$(src)
CFLAGS_mt76x02_trace.o := -I$(src)
CFLAGS_mt792x_trace.o := -I$(src)

# --- Chipset families (subdirectories) ---
# MT7603 (WiFi 4), MT7915/MT7916 (WiFi 6 PCIe/SoC), and MT7996 (WiFi 7 PCIe)
# are not built by default. No firmware is shipped for these chips in this
# repo (see info/MARKET-FOOTPRINT.md and firmware/README.md), so users of
# this out-of-tree driver do not need them compiled. The source trees are
# retained so anyone with matching hardware can reactivate by uncommenting
# the matching line below.
#obj-m += mt7603/
obj-m += mt7615/
#obj-m += mt7915/
obj-m += mt7921/
obj-m += mt7925/
#obj-m += mt7996/
obj-m += mt76x0/
obj-m += mt76x2/

else

# ============================================================
# User section -- targets for building from the command line
# ============================================================

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
MODDIR ?= /lib/modules/$(KVER)/extra/mt76
FWDIR := /lib/firmware/mediatek
NPROC ?= $(shell nproc --ignore=1)

.PHONY: modules clean install install_fw uninstall cleanup_target_system

modules:
	$(MAKE) -j$(NPROC) -C $(KDIR) M=$$PWD modules

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean

# Remove conflicting in-kernel mt76 modules
#
# WARNING: This permanently deletes in-kernel modules. They will NOT be
# restored by a kernel update -- only a full kernel package reinstall will
# bring them back. In most cases the blacklist in mt76_git.conf is sufficient
# and this target is unnecessary. Only use this if the blacklist is not working.
cleanup_target_system:
	@printf '\n  WARNING: This will permanently delete in-kernel mt76 modules.\n'
	@printf '  The blacklist (mt76_git.conf) is the recommended approach.\n'
	@printf '  Only proceed if the blacklist is not working for you.\n\n'
	@printf '  Target kernel: %s\n\n' "$(KVER)"
	@printf '  Type YES to continue: ' && read ans && [ "$$ans" = "YES" ] || { echo "Aborted."; exit 1; }
	@echo "Removing conflicting in-kernel mt76 modules..."
	find /lib/modules/$(KVER)/kernel -name "mt76*.ko*" -exec rm -fv {} \;
	find /lib/modules/$(KVER)/kernel -name "mt7603*.ko*" -exec rm -fv {} \;
	find /lib/modules/$(KVER)/kernel -name "mt7615*.ko*" -exec rm -fv {} \;
	find /lib/modules/$(KVER)/kernel -name "mt7663*.ko*" -exec rm -fv {} \;
	find /lib/modules/$(KVER)/kernel -name "mt7915*.ko*" -exec rm -fv {} \;
	find /lib/modules/$(KVER)/kernel -name "mt7921*.ko*" -exec rm -fv {} \;
	find /lib/modules/$(KVER)/kernel -name "mt7925*.ko*" -exec rm -fv {} \;
	find /lib/modules/$(KVER)/kernel -name "mt7996*.ko*" -exec rm -fv {} \;
	find /lib/modules/$(KVER)/kernel -name "mt792x*.ko*" -exec rm -fv {} \;
	depmod -a $(KVER)

install:
	@echo "Installing mt76_git modules to $(MODDIR)..."
	@find . -name "*_git.ko" -exec strip -g {} \;
	@install -dvm 755 $(MODDIR)
	@find . -name "*_git.ko" -exec install -vm 644 {} $(MODDIR) \;
	@# Match the distro's module compression scheme
	@if ls /lib/modules/$(KVER)/kernel/net/wireless/*.ko.zst >/dev/null 2>&1; then \
		echo "Compressing modules with zstd (matching distro scheme)..."; \
		zstd -fq --rm $(MODDIR)/*.ko 2>/dev/null || true; \
	elif ls /lib/modules/$(KVER)/kernel/net/wireless/*.ko.xz >/dev/null 2>&1; then \
		echo "Compressing modules with xz (matching distro scheme)..."; \
		xz -f $(MODDIR)/*.ko 2>/dev/null || true; \
	elif ls /lib/modules/$(KVER)/kernel/net/wireless/*.ko.gz >/dev/null 2>&1; then \
		echo "Compressing modules with gzip (matching distro scheme)..."; \
		gzip -f $(MODDIR)/*.ko 2>/dev/null || true; \
	fi
	depmod -a $(KVER)

install_fw:
	@echo "Installing firmware to $(FWDIR)..."
	@# Root-level firmware (mt76x0, mt76x2, mt792x family)
	@install -dvm 755 $(FWDIR)
	@for f in firmware/*.bin; do \
		[ -f "$$f" ] || continue; \
		install -vm 644 "$$f" $(FWDIR)/; \
	done
	@# Subdirectory firmware (mt7925/, mt7996/)
	@for subdir in firmware/*/; do \
		[ -d "$$subdir" ] || continue; \
		dirname=$$(basename "$$subdir"); \
		install -dvm 755 $(FWDIR)/$$dirname; \
		for f in $$subdir*.bin; do \
			[ -f "$$f" ] || continue; \
			install -vm 644 "$$f" $(FWDIR)/$$dirname/; \
		done; \
	done
	@# Match distro compression scheme
	@if ls /lib/modules/$$(uname -r)/kernel/net/wireless/*.ko.zst >/dev/null 2>&1; then \
		echo "Compressing firmware with zstd (matching distro scheme)..."; \
		find $(FWDIR) -name '*.bin' -exec zstd -fq --rm {} \; 2>/dev/null || true; \
	elif ls /lib/modules/$$(uname -r)/kernel/net/wireless/*.ko.xz >/dev/null 2>&1; then \
		echo "Compressing firmware with xz (matching distro scheme)..."; \
		find $(FWDIR) -name '*.bin' -exec xz -f -C crc32 {} \; 2>/dev/null || true; \
	elif ls /lib/modules/$$(uname -r)/kernel/net/wireless/*.ko.gz >/dev/null 2>&1; then \
		echo "Compressing firmware with gzip (matching distro scheme)..."; \
		find $(FWDIR) -name '*.bin' -exec gzip -f {} \; 2>/dev/null || true; \
	fi
	@echo "Firmware install complete."

uninstall:
	@echo "Removing mt76_git modules from $(MODDIR)..."
	@rm -rvf $(MODDIR)
	@rmdir -v --ignore-fail-on-non-empty /lib/modules/$(KVER)/extra 2>/dev/null || true
	depmod -a $(KVER)

endif
