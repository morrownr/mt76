# mt76 Out-of-Tree WiFi Driver Build Infrastructure

Build, install, and manage MediaTek mt76 WiFi drivers as out-of-tree kernel
modules. Supports DKMS, non-DKMS, Secure Boot, and kernels 6.6 through 7.x.

Based on [openwrt/mt76](https://github.com/openwrt/mt76), adapted for
standalone out-of-tree building by the
[morrownr](https://github.com/morrownr) community.

## Quick Start

```sh
# Install
git clone https://github.com/morrownr/mt76.git
cd mt76
sudo sh install-driver.sh

# Check status (no root needed)
./check-driver.sh

# Update
git pull
sudo sh install-driver.sh

# Uninstall
sudo sh uninstall-driver.sh
```

## Supported Chipset Families

| Family | Bus | Modules | WiFi Generation |
|--------|-----|---------|-----------------|
| MT7603 | PCIe | mt7603e | WiFi 4 (N) |
| MT76x0 | USB, PCIe | mt76x0u, mt76x0e | WiFi 5 (AC) |
| MT76x2 | USB, PCIe | mt76x2u, mt76x2e | WiFi 5 (AC) |
| MT7615 | PCIe, USB, SDIO | mt7615e, mt7663u, mt7663s | WiFi 5 (AC) |
| MT7915 | PCIe | mt7915e | WiFi 6 (AX) |
| MT7921 | PCIe, USB, SDIO | mt7921e, mt7921u, mt7921s | WiFi 6E (AXE) |
| MT7925 | PCIe, USB | mt7925e, mt7925u | WiFi 7 (BE) |
| MT7996 | PCIe | mt7996e | WiFi 7 (BE) |

## Supported USB Adapters

**MT76x0 (WiFi 5 AC)**

| VID:PID | Device |
|---------|--------|
| 148f:7610 | MT7610U reference design |
| 13b1:003e | Linksys AE6000 |
| 0e8d:7610 | Sabrent NTWLAC |
| 7392:a711 | Edimax 7711mac |
| 7392:b711 | Edimax / Elecom |
| 148f:761a | TP-Link TL-WDN5200 |
| 0b05:17d1 | Asus USB-AC51 |
| 0b05:17db | Asus USB-AC50 |
| 0df6:0075 | Sitecom WLA-3100 |
| 2019:ab31 | Planex GW-450D |
| 2001:3d02 | D-Link DWA-171 rev B1 |
| 0586:3425 | Zyxel NWD6505 |
| 07b8:7610 | AboCom AU7212 |
| 04bb:0951 | I-O DATA WN-AC433UK |
| 057c:8502 | AVM FRITZ!WLAN AC 430 |
| 293c:5702 | Comcast Xfinity KXW02AAA |
| 20f4:806b | TRENDnet TEW-806UBH |
| 7392:c711 | Devolo WiFi AC Stick |
| 2357:0123 | TP-Link T2UHP v1 |
| 2357:0105 | TP-Link Archer T1U |

**MT76x2 (WiFi 5 AC)**

| VID:PID | Device |
|---------|--------|
| 0b05:1833 | Asus USB-AC54 |
| 0b05:17eb | Asus USB-AC55 |
| 0b05:180b | Asus USB-N53 B1 |
| 0e8d:7612 | Aukey USBAC1200 / Alfa AWUS036ACM |
| 057c:8503 | AVM FRITZ!WLAN AC860 |
| 7392:b711 | Edimax EW-7722UAC |
| 0e8d:7632 | HC-M7662BU1 |
| 2c4e:0103 | Mercury UD13 |
| 0846:9014 | Netgear WNDA3100v3 |
| 0846:9053 | Netgear A6210 |
| 045e:02e6 | Xbox One Wireless Adapter |
| 045e:02fe | Xbox One Wireless Adapter |
| 0471:2126 | LiteOn WN4516R |
| 0471:7600 | LiteOn WN4519R |
| 2357:0137 | TP-Link TL-WDN6200 |

**MT7663 (WiFi 5 AC)**

| VID:PID | Device |
|---------|--------|
| 0e8d:7663 | MT7663 reference (USB) |
| 043e:310c | LG MT7663 (USB) |

**MT7921 (WiFi 6E AXE)**

| VID:PID | Device |
|---------|--------|
| 0e8d:7961 | MT7921AU reference |
| 3574:6211 | D-Link DWA-X1850 |
| 0846:9060 | Netgear AXE3000 (A8000) |
| 0846:9065 | Netgear AXE3000 (A8000S) |
| 35bc:0107 | 8Devices USB-WiFi6E |

**MT7925 (WiFi 7 BE)**

| VID:PID | Device |
|---------|--------|
| 0e8d:7925 | MT7925 reference |
| 0846:9072 | Netgear (WiFi 7) |

## Supported PCIe Devices

| Family | PCI IDs (vendor 14c3 unless noted) |
|--------|-----|
| MT7603 | 7603 |
| MT76x0 | 7610, 7630, 7650 |
| MT76x2 | 7662, 7612, 7602 |
| MT7615 | 7615, 7663, 7611 |
| MT7915 | 7915, 7906, 7916, 790a |
| MT7921 | 7961, 7922, 0608, 0616, 7920, 7902 (also 0b48:7922) |
| MT7925 | 7925, 0717 |
| MT7996 | 7990, 7991, 7992, 799a, 7993, 799b |

## Files

| File | Purpose |
|------|---------|
| `Makefile` | Kbuild out-of-tree Makefile for all 29 modules across 8 chipset families |
| `install-driver.sh` | Full install with progress display, prerequisite checks, build, verify |
| `uninstall-driver.sh` | Clean removal of installed modules, DKMS entries, and config |
| `check-driver.sh` | No-root diagnostic: loaded modules, hardware, link status, firmware, dmesg |
| `dkms.conf` | DKMS auto-rebuild on kernel updates, conditional PCI/SDIO support |
| `mt76_git.conf` | modprobe options and blacklist for all 29 in-kernel mt76 modules |
| `compat-patches/` | Source compatibility patches for kernels 6.6 through 6.19+ |
| `mt7603/Makefile` | MT7603 PCIe module (WiFi 4) |
| `mt7615/Makefile` | MT7615 family: common + PCIe + USB + SDIO (WiFi 5) |
| `mt7915/Makefile` | MT7915 PCIe with coredump support (WiFi 6) |
| `mt7921/Makefile` | MT7921 family: common + PCIe + USB + SDIO (WiFi 6E) |
| `mt7925/Makefile` | MT7925 family: common + PCIe + USB (WiFi 7) |
| `mt7996/Makefile` | MT7996 PCIe with NPU and coredump support (WiFi 7) |
| `mt76x0/Makefile` | MT76x0 family: common + USB + PCIe (WiFi 5) |
| `mt76x2/Makefile` | MT76x2 family: common + USB + PCIe (WiFi 5) |

## Module Map (29 modules)

```
Core and transport:
  mt76_git              Core driver
  mt76_usb_git          USB transport
  mt76_sdio_git         SDIO transport (requires CONFIG_MMC)

Shared libraries:
  mt76x02_lib_git       Legacy shared (MT76x0/x2)
  mt76x02_usb_git       Legacy USB shared
  mt76_connac_lib_git   CONNAC shared
  mt792x_lib_git        MT792x shared
  mt792x_usb_git        MT792x USB shared

Chipset families:
  mt7603e_git           MT7603 PCIe (requires CONFIG_PCI)
  mt7615_common_git     MT7615 shared
  mt7615e_git           MT7615 PCIe
  mt7663_usb_sdio_common_git  MT7663 USB/SDIO shared
  mt7663u_git           MT7663 USB
  mt7663s_git           MT7663 SDIO (requires CONFIG_MMC)
  mt7915e_git           MT7915 PCIe
  mt7921_common_git     MT7921 shared
  mt7921e_git           MT7921 PCIe
  mt7921s_git           MT7921 SDIO
  mt7921u_git           MT7921 USB
  mt7925_common_git     MT7925 shared
  mt7925e_git           MT7925 PCIe
  mt7925u_git           MT7925 USB
  mt7996e_git           MT7996 PCIe
  mt76x0_common_git     MT76x0 shared
  mt76x0u_git           MT76x0 USB
  mt76x0e_git           MT76x0 PCIe
  mt76x2_common_git     MT76x2 shared
  mt76x2u_git           MT76x2 USB
  mt76x2e_git           MT76x2 PCIe
```

## How It Works

All out-of-tree modules are built with a `_git` suffix (e.g., `mt76_git.ko`
instead of `mt76.ko`). This allows them to coexist with the in-kernel mt76
modules. The blacklist in `mt76_git.conf` prevents the in-kernel versions
from loading, so the out-of-tree versions handle all hardware.

This means:
- **Safe to install** -- your in-kernel modules are untouched
- **Safe to uninstall** -- blacklist removal restores in-kernel behavior
- **Safe to update** -- just `git pull && sudo sh install-driver.sh`

## Prerequisites

The installer checks for all of these automatically:

- `gcc` -- C compiler
- `make` -- build tool
- `bc` -- used by kernel build system
- `iw` -- wireless configuration tool
- Kernel header files for your running kernel
- Optional: `dkms` for automatic rebuilds on kernel updates

## Reporting Issues

Run `check-driver.sh` and include the full output in your bug report:

```sh
./check-driver.sh 2>&1 | tee mt76-diag.txt
```

This captures loaded modules (in-kernel vs out-of-tree), hardware detection,
link status with signal strength, firmware presence, blacklist health, Secure
Boot status, and recent kernel messages. Everything a maintainer needs in one
command.

## Kernel Compatibility

Supported range: **6.6 through 7.x** (enforced by `BUILD_EXCLUSIVE_KERNEL`
in `dkms.conf`).

Compatibility patches in `compat-patches/`:
- `linux/unaligned.h` vs `asm/unaligned.h` header rename (kernel 6.12)
- `hrtimer_setup()` replacing `hrtimer_init()` (kernel 6.12/6.19)
- OpenWrt-specific EMLSR API isolation

## Design Decisions

- **_git suffix**: Follows the morrownr/rtw89 convention. Modules install to
  `extra/` or `updates/` directories which take priority over `kernel/` in
  depmod search order.
- **PCI conditional**: PCIe-only modules gated on `CONFIG_PCI`
- **SDIO conditional**: SDIO modules gated on `CONFIG_MMC`
- **USB unconditional**: USB modules always built
- **Git commit in modules**: `-DGIT_COMMIT` baked in at build time for traceability
- **Module compression**: Install target auto-matches distro compression (zstd/xz/gzip)
- **No SoC code**: MT7622_WMAC, MT798X_WMAC, NPU configs left as no-ops (never
  defined on desktop/laptop kernels, code compiles out)

## License

This program is free software; you can redistribute it and/or modify it under
the terms of version 2 of the GNU General Public License as published by the
Free Software Foundation.
