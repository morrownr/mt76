# MT7927 Multi-Link Operation (MLO)

How the MT7927 MLO support behaves for an ordinary user. Nothing here needs a
module option, a pinned BSSID, a debugfs write or a patched mac80211; build and
install the driver as described in the README.

## What connects, and how

| Access point | Connection |
|--------------|------------|
| Wi-Fi 7 AP with MLO, MLO-capable `wpa_supplicant` (hostap with MLO support) | One MLD association over two links |
| Wi-Fi 7 AP, supplicant without MLO support | Ordinary single-link connection |
| Wi-Fi 6E / 6 / 5 / 4 AP | Ordinary single-link connection, unchanged |

The MT7927 has two radios: slot 0 is the 2.4 GHz radio and slot 1 is shared by
5 GHz and 6 GHz. An MLD therefore carries at most one of 5 GHz and 6 GHz next
to 2.4 GHz, and the two link pairs are:

* `0x5` : 2.4 GHz + 6 GHz
* `0x3` : 2.4 GHz + 5 GHz

## Link pair selection

The pair follows the band the supplicant associates on (the anchor): a 6 GHz
anchor gives `0x5`, a 5 GHz anchor gives `0x3`. A 2.4 GHz anchor works too and
pairs with the better of the other two bands, but 2.4 GHz is the slowest band,
so the pair is only as good as that link; the supplicant normally ranks it
last.

Moving between `0x5` and `0x3` while keeping the association needs the 2.4 GHz
link as the anchor in both pairs. With a 5 GHz or 6 GHz anchor the same change
is an anchor change: a clean disconnect and a new association, which the
supplicant re-ranks by itself. The driver rate limits this (`120 s` minimum
interval, doubling up to `30 min` after repeated degradation).

## Steering and fallback

Steering starts by itself once the AP station is authorized
(`mlo_auto_steer`, default `Y`). It reacts to events (a link losing signal,
a failing link), not to a timer. When the active pair degrades:

1. A transition that has already been proven in the current association is
   taken directly.
2. A transition that has not been proven is blocked
   (`class_not_verified_in_this_association`) unless `mlo_steer_try_unverified=Y`,
   which is for experiments only.
3. The surviving link keeps the association and traffic (single-link fallback);
   the pair is rebuilt on the next clean association.

Because the surviving link is kept, the TCP flow and the association continue
through a link loss.

## Module options of interest

| Option | Default | Meaning |
|--------|---------|---------|
| `mlo_join_wait_bss` | `Y` | Wait for the partner link to be registered before the MLO join, so any band can be the anchor |
| `mlo_auto_steer` | `Y` | Start steering after authorization |
| `mlo_steer_try_unverified` | `N` | Attempt transitions not yet proven in this association |
| `mlo_tx_primary_handle` | `1` | Address MLD unicast data to the primary active link WCID |

The `test_*` options are for driver development and are not meant to be set.

## Known limits

* The firmware distributes an MLD's frames over the links itself. How much
  of the downlink the AP puts on each link is the AP's decision.
* The 6 GHz link on a 320 MHz channel has shown weak downlink throughput in
  some environments, with and without MLO. When a 5 GHz anchor is available it
  gives the better downlink.
* An AP that still holds a previous MLD association for the station rejects a
  new one with status 30 until it times out; disconnect cleanly before
  reconnecting.
