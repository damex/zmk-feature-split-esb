# zmk-feature-split-esb

Enhanced ShockBurst (2.4 GHz) split transport for ZMK. One or more peripherals, one central.
Packet-native: split messages map to ESB packets (a report's input events
coalesce into one), carried over a lock-free SPSC RX path.
ESB hardware ACK + retransmit + CRC handle reliability.

## Install

Add it to your `config/west.yml`. `import: true` pulls the deps a vanilla ZMK
workspace lacks (`zmk` and Zephyr come from your own manifest):
```yaml
  remotes:
    - name: damex
      url-base: https://github.com/damex
  projects:
    - name: zmk-feature-split-esb
      remote: damex
      revision: v0.1.1
      import: true
```
Then update and apply the Kconfig fixes `sdk-nrf` needs on ZMK's Zephyr:
```
west update
west patch -sm zmk-feature-split-esb apply
```
For a local checkout, build with `-DZMK_EXTRA_MODULES=<path>/zmk-feature-split-esb`
instead. Your workspace must then already provide `sdk-nrf` + `nrfxlib` with the
patches applied.

## Configure

Board/shield conf, select ESB and drop the other transports:
```conf
CONFIG_ZMK_SPLIT=y
CONFIG_ZMK_SPLIT_BLE=n
CONFIG_ZMK_SPLIT_WIRED=n
CONFIG_ZMK_SPLIT_ESB=y
```
Central also sets `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`. Peripheral leaves it unset.

Same link identity in a dtsi included by the central and every peripheral:
```dts
esb_link {
    compatible = "zmk,split-esb";
    base-address = [E7 E7 E7 E7];
    hop-channels = [04];
    peripherals {
        mouse: peripheral_mouse {
            pipe = <0>;
            prefix = <0xE7>;
            weight = <1>;
        };
    };
};
```
Each peripheral board points at its entry: `chosen { zmk,esb-self = &mouse; };`.
More peripherals: add another child, each with the next `pipe` (1, 2, ...) and a unique `prefix`.

| DT property | Value |
|---|---|
| `base-address` | 4-byte bytestring `[..]`, shared by all pipes |
| `peripherals` | one child node per peripheral: `pipe`, `prefix` (1 byte), `weight` |
| `hop-channels` | channel bytestring, each 0-100 (2400 + N MHz). 1 = fixed, 2+ = hopping set |
| `hop-threshold` | bad windows before acting: central hop-vote sum, peripheral sweep streak (default 3) |
| `hop-window-ms` | peripheral keepalive period while data flows (default 32) |
| `idle-keepalive-ms` | peripheral idle keepalive period, also central hop-decision window (default 128) |
| `tx-power-dbm` | boot TX power in dBm, raise for range (default 0) |
| `retransmit-count` | retransmits before drop (default 3) |
| `retransmit-delay-us` | delay between retransmits (default 600) |
| `use-fast-ramp-up` | shorter radio ramp-up, nRF52/nRF53, all peripherals must match the central |
| `crc-bits` | CRC width 0/8/16, all peripherals must match the central (default 16) |
| `bitrate-kbps` | radio bitrate 1000/2000, all peripherals must match the central (default 2000) |
| `lossy-codes` | optional list of `<INPUT_EV_* code>` pairs sent without ACK |

Lossy-codes lists the input axes peripherals fire-and-forget. Reserve for
high-rate, self-correcting axes (pointer motion). Non-input split events
(key-position, sensor, battery) are always ACK'd. Every input event is ACK'd
unless its (type, code) is listed here. Omitted = fully lossless link. Example
for a mouse:
```dts
#include <zephyr/dt-bindings/input/input-event-codes.h>
&esb_link {
    lossy-codes
        = <INPUT_EV_REL INPUT_REL_X>
        , <INPUT_EV_REL INPUT_REL_Y>;
};
```

Tunables (Kconfig, defaults shown):

| Option | Default | Notes |
|---|---|---|
| `ZMK_SPLIT_ESB_MAX_PAYLOAD` | 48 | max on-air bytes (>= largest split msg) |
| `ZMK_SPLIT_ESB_RX_QUEUE_SIZE` | 16 | RX SPSC ring depth (power of 2) |
| `ZMK_SPLIT_ESB_RX_THREAD_STACK_SIZE` | 1536 | RX dispatch thread stack |
| `ZMK_SPLIT_ESB_RX_THREAD_PRIORITY` | 2 | RX dispatch thread priority |
| `ZMK_SPLIT_ESB_REPLY_QUEUE_SIZE` | 8 | central reverse-channel queue depth |
| `ZMK_SPLIT_ESB_TX_FIFO_SIZE` | 8 | radio TX FIFO depth |
| `ZMK_SPLIT_ESB_RX_FIFO_SIZE` | 8 | radio RX FIFO depth |
| `ZMK_SPLIT_ESB_PRIORITY` | 50 | transport registration priority |

## Channel hopping

List two or more channels in `hop-channels` and the link hops between them, stepping
off any channel that gets noisy. One channel is a fixed link, no hopping. Every
peripheral must carry the central's list, so flash them as a set.

## Load order

Each step overrides the previous:

1. **Boot defaults:** the DT props program the radio at startup.
2. **Saved values:** any persisted in NVS apply on top (needs `CONFIG_SETTINGS`).
3. **Live changes:** pushed at runtime via `settings_runtime_set`, no reflash.

Runtime-overridable keys: `esb/tx_power`, `esb/retransmit_count`, `esb/retransmit_delay`.

## Limitations

`ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT` is 0 on an ESB-only central (ZMK derives it
from BLE/wired counts). Events still deliver, but:

- Commands to the peripheral need `BEHAVIOR_LOCALITY_EVENT_SOURCE`, source = its id.
- No `BEHAVIOR_LOCALITY_GLOBAL` broadcast or HID-indicator forwarding (both index that count).

## License

This module is MIT.

Workspace dependencies pulled by `west.yml`. Each keeps its own license:

| Dependency | License |
|---|---|
| ZMK | MIT |
| Zephyr | Apache-2.0 |
| sdk-nrf | LicenseRef-Nordic-5-Clause |
| nrfxlib | LicenseRef-Nordic-5-Clause (parts this module's builds touch: MPSL, softdevice_controller) |

The `LicenseRef-Nordic-5-Clause` parts restrict use to Nordic hardware.

For an ESB-only build the link symbols come from sdk-nrf.
nrfxlib is still cloned into your workspace and gets linked if you also enable BLE.

Patches under `zephyr/patches/nrf/` modify sdk-nrf files (Nordic-licensed) and
apply on the user side via `west patch`. They don't redistribute Nordic source.
`patches.yml` lists upstream versions after which each patch can be dropped.
