# MT7927 monitor-v3 hardware reproduction

This bundle pins candidate `ca013156cfc2e7641e273a5ea0f6ece896facd43`
on Linux `7.2.0-rc1`. No script builds, installs, reboots, or removes a kernel
unless its explicit `--execute` option is supplied. No script reboots the host.

## 1. Build without activation

Review the plan first:

```sh
./build-kernel.sh
```

Build separate Debian packages only when ready:

```sh
BUILD_DIR=/home/pi/mt7927-kernel-build JOBS=2 ./build-kernel.sh --execute
```

The build uses the running distro kernel config and the local version
`-mt7927-monitor-v3`. Do not continue unless the build exits successfully.

## 2. Record the rollback baseline

Before installation, record:

```sh
uname -r
dpkg-query -W 'linux-image*' 'linux-headers*'
```

Keep the current distro kernel installed. The test kernel must be an additional
boot entry, not a replacement.

## 3. Install without rebooting

Point `PACKAGE_DIR` at the directory containing the generated packages:

```sh
PACKAGE_DIR=/path/to/packages ./install-kernel.sh
sudo PACKAGE_DIR=/path/to/packages ./install-kernel.sh --execute
```

Installation updates GRUB. Check `GRUB_DEFAULT` and the generated menu before
rebooting: with `GRUB_DEFAULT=0`, the newest candidate becomes the next default;
otherwise select `7.2.0-rc1-mt7927-monitor-v3` manually. Reboot manually. After
boot, verify `uname -r` contains `mt7927-monitor-v3`, `lspci -nnk` binds MT7927
to `mt7925e`, and `iw dev` shows the expected radio interfaces.

An unrelated DKMS module can reject a new kernel API and leave the image package
half-configured. Do not alter the MT7927 series to hide that failure. Either fix
the unrelated module or use a kernel-specific DKMS override only after confirming
that module is not required by the test host, then finish `dpkg --configure`.

## 4. Capture baseline evidence

Save these before the first transition and again after every failure:

```sh
uname -a
lspci -nnk
iw phy
iw dev
iw reg get
sudo dmesg --ctime
```

Create an `iw phy PHY info` capture for the matrix. The monitor test visits all
enabled channels reported by cfg80211; it does not use a fixed channel list.
DFS and no-IR frequencies are passive-monitor checks only. Packet TX excludes
disabled, radar-detection, and no-IR frequencies.

## 5. Reproduction matrix

Use only an owned, isolated lab and receiver. First inspect the dry run:

```sh
MONITOR_PHY=phy0 INJECTION_RECEIVER=02:00:00:00:00:01 \
  ./run-hardware-matrix.sh --iw-list iw-phy-info.txt
```

Configure the receiver-side capture and packet-TX status commands described by
`tools/injection-matrix.sh --help`, then run with `--execute`. The required
coverage is:

- 2.4 -> 5 -> 6 -> 5 -> 2.4 GHz transitions;
- every cfg80211-enabled channel in each available band;
- passive monitor while a managed VIF remains active;
- monitor-only operation;
- chanctx remove/re-add and monitor-interface recreation;
- continuously increasing packet counts at every channel step;
- receiver-side tagged packet evidence on each TX-capable band/channel;
- TX completion evidence with no stale or failed status;
- no firmware recovery, reset loop, warning, or error in `dmesg`.

Record unsupported channels as regulatory skips, not driver passes. Mark only
the rows with captured evidence as verified. MLO/STR and simultaneous tri-band aggregation remain unverified.

## 6. Rollback

If the candidate fails, reboot manually and select the previously recorded
distro kernel. Confirm `uname -r` no longer contains `mt7927-monitor-v3` before
removing the candidate packages:

```sh
./rollback-kernel.sh PACKAGE_NAME...
sudo ./rollback-kernel.sh --execute PACKAGE_NAME...
```

Keep all logs and captures with the candidate commit and kernel release. Do not
prepare or send an upstream series until the required matrix rows reproduce.
