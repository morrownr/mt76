# MT7927 Driver Install

These steps start from a clean checkout and install the out-of-tree MT7927
driver modules with the `_git` suffix.

## Install

```sh
sudo apt update
sudo apt install -y git build-essential bc iw kmod linux-headers-$(uname -r)

git clone https://github.com/Ashcal9669/mt76.git
cd mt76

make -j$(nproc)
sudo make install

sudo modprobe -r mt7927e_git mt7927_common_git mt792x_lib_git mt76_connac_lib_git mt76_git 2>/dev/null || true
sudo modprobe mt7927e_git
```

If the module is already installed and only needs to be refreshed after pulling
new commits:

```sh
git pull
make -j$(nproc)
sudo make install
sudo modprobe -r mt7927e_git mt7927_common_git mt792x_lib_git mt76_connac_lib_git mt76_git 2>/dev/null || true
sudo modprobe mt7927e_git
```

## Verify The Driver Loaded

```sh
lsmod | grep -E 'mt7927|mt792x|mt76'
iw dev
sudo dmesg | grep -iE 'mt7927|firmware|ASIC' | tail -80
```

The expected Wi-Fi module is `mt7927e_git`. The expected netdev name depends on
the system, but common names are `wlp6s0` or similar.

## Monitor Mode / hcxdumptool

Use the real netdev for monitor mode. Do not create a separate `mon0` while the
same PHY still has managed, P2P, or MLO state.

```sh
sudo systemctl stop wpa_supplicant NetworkManager iwd 2>/dev/null || true
sudo iw reg set US

sudo ip link set dev wlp6s0 down
sudo iw dev wlp6s0 set type monitor
sudo ip link set dev wlp6s0 up

iw dev
sudo hcxdumptool -i wlp6s0 --rcascan=active --rds=5 -F
```

A successful run should show beacons captured, probe requests transmitted, probe
responses received, and zero kernel packet drops in the final hcxdumptool
summary.

## Return To Managed Mode

```sh
sudo ip link set dev wlp6s0 down
sudo iw dev wlp6s0 set type managed
sudo ip link set dev wlp6s0 up
sudo systemctl start NetworkManager 2>/dev/null || true
sudo systemctl start wpa_supplicant 2>/dev/null || true
```
