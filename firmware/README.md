2026-04-19

Mediatek Firmware Files

The purpose of this document is to show which mt76 firmware files are
included in this repo and what the filenames are for each of the mt76
chips.

The following links provide information to help understand the order
of this document and where updated files can be obtained:

[Mediatek Official Linux Website that outlines supported chips](https://wireless.docs.kernel.org/en/latest/en/users/drivers/mediatek.html)

[Mediatek Official Linux firmware repository](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mediatek)

Information regarding the mt76 [MARKET-FOOTPRINT](https://github.com/morrownr/mt76/blob/main/info/MARKET-FOOTPRINT.md0)

Note: I use linux-wireless for notifications of new firmware files. The
log at the above Mediatek Official Linux firmware repository link can
also be used.

-----

MT7610U 802.11a/b/g/n/ac 1T1R 2.4/5GHz USB Chip  (4.19+)

Status: firmware included, driver compiled

Binary firmware for MT7610U WiFi devices

```
File: mediatek/mt7610u.bin
```

Available in Linux firmware repository since: 2018-08-14

-----

MT7612/MT7602/MT7662 802.11a/b/g/n/ac 2T2R 2.4/5GHz PCIe/USB Chip (4.19+)

Status: firmware included, driver compiled

Binary firmware for MT76x2u WiFi devices

```
File: mediatek/mt7662u.bin
File: mediatek/mt7662u_rom_patch.bin
```

Available in Linux firmware repository since: 2018-07-30

-----

MT7630E 802.11a/b/g/n 1T1R 2.4/5GHz PCIe Chip

Status: no firmware, not compiled

-----

MT7610E 802.11a/b/g/n/ac 1T1R 2.4/5GHz PCIe Chip (4.19+)

Status: firmware included, driver compiled

Binary firmware for MT7610E WiFi devices 

```
File: mediatek/mt7610e.bin
```

Available in Linux firmware repository since: 2018-10-18

-----

MT7603E 802.11b/g/n 2T2R 2.4GHz PCIe chip and
MT7628 802.11b/g/n 2T2R 2.4GHz SoC Device (4.7+)

Status: no firmware, not compiled

-----

MT7615 802.11a/b/g/n/ac 4T4R 2.4/5GHz PCIe Chip (5.2+)

Status: no firmware, not compiled

-----

MT7622 802.11b/g/n 4T4R 2.4GHz SoC Device (5.7+)

Status: no firmware, not compiled

-----

MT7663 802.11a/b/g/n/ac 2T2R 2.4/5GHz PCIe/USB/SDIO Chip (5.8+)

Status: firmware included, driver compiled

Binary firmware for MT7663 WiFi devices 

```
File: mediatek/mt7663_n9_v3.bin	
File: mediatek/mt7663pr2h.bin
File: mediatek/mt7663_n9_rebb.bin
File: mediatek/mt7663pr2h_rebb.bin
```

Available in Linux firmware repository since: 2020-04-13

-----

MT7915/MT7916 802.11a/b/g/n/ac/ax 4T4R 2.4/5GHz PCIe Chip (5.9+)

Status: no firmware, not compiled

-----

MT7986/MT7981 802.11a/b/g/n/ac/ax 4T4R 2.4/5GHz SoC Device (5.18+)

Status: no firmware, not compiled

-----

MT7921 802.11a/b/g/n/ac/ax 2T2R 2.4/5GHz/6Hz PCIe/USB/SDIO Chip

Status: firmware included, drivers compiled

Note: The MT7902, MT7920, MT7921 and MT7922 chips are all supported 
by the mt7921u/e/s driver but have their own firmware files.

Binary firmware for MT7902 WiFi devices

```
File: mediatek/WIFI_MT7902_patch_mcu_1_1_hdr.bin
File: mediatek/WIFI_RAM_CODE_MT7902_1.bin
```

Available in Linux firmware repository since: 2026-02-21

Binary firmware for MT7920 WiFi devices

```
File: mediatek/WIFI_MT7961_patch_mcu_1a_2_hdr.bin
File: mediatek/WIFI_RAM_CODE_MT7961_1a.bin
```

Available in Linux firmware repository since: 2024-10-01

Binary firmware for MT7921 WiFi devices

```
File: mediatek/WIFI_MT7961_patch_mcu_1_1_hdr.bin
File: mediatek/WIFI_RAM_CODE_MT7961_1.bin
```

Available in Linux firmware repository since: 2021-02-08 

Binary firmware for MT7922 WiFi devices

```
File: mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin
File: mediatek/WIFI_RAM_CODE_MT7922_1.bin
```

Available in Linux firmware repository since: 2021-08-12 

-----

MT7996 802.11a/b/g/n/ac/ax/be 4T4R 2.4/5G/6GHz PCIe Chip (6.2+)

Status: no firmware, not compiled

-----

MT7925 802.11a/b/g/n/ac/ax/be 2T2R 2.4/5G/6GHz PCIe/USB Chip (6.7+)

Binary firmware for MT7925 WiFi devices

```
File: mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin
File: mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.binBB
```

Available in Linux firmware repository since: 2024-01-02

-----

MT7992 802.11a/b/g/n/ac/ax/be 4T4R 2.4/5T5R 5G PCIe Chip (6.8+)

Status: no firmware, not compiled

-----

Projected new driver/firmware: MT7927

-----
