# zmk-feature-split-esb

Enhanced ShockBurst (2.4 GHz) split transport for ZMK. One peripheral, one central.
Packet-native: one ESB packet = one split message; ESB hardware ACK + retransmit +
CRC handle reliability.

## Install

Add it to your `config/west.yml`; `import: true` pulls the deps a vanilla ZMK
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
instead; your workspace must then already provide `sdk-nrf` + `nrfxlib` with the
patches applied.

## Configure

Board/shield conf, select ESB and drop the other transports:
```conf
CONFIG_ZMK_SPLIT=y
CONFIG_ZMK_SPLIT_BLE=n
CONFIG_ZMK_SPLIT_WIRED=n
CONFIG_ZMK_SPLIT_ESB=y
```
Central also sets `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`; peripheral leaves it unset.

Same link identity in a dtsi included by both sides:
```dts
esb_link: esb_link {
    compatible = "zmk,split-esb";
    base-address = [E7 E7 E7 E7];
    prefix = <0xC2>;
    rf-channel = <4>;
};
```

| DT property | Value |
|---|---|
| `base-address` | 4-byte bytestring `[..]`, pipe 0 |
| `prefix` | 1 byte, pipe 0 |
| `rf-channel` | 0-100 (2400 + N MHz) |

Tunables (Kconfig, defaults shown):

| Option | Default | Notes |
|---|---|---|
| `ZMK_SPLIT_ESB_BITRATE_2MBPS` / `ZMK_SPLIT_ESB_BITRATE_1MBPS` | 2 Mbps | link rate |
| `ZMK_SPLIT_ESB_TX_POWER_DBM` | 0 | boot TX power; raise for range |
| `ZMK_SPLIT_ESB_RETRANSMIT_COUNT` | 3 | retransmits before drop |
| `ZMK_SPLIT_ESB_RETRANSMIT_DELAY_US` | 600 | delay between retransmits |
| `ZMK_SPLIT_ESB_LOSSY_INPUT` | n | input without ACK (lower latency) |
| `ZMK_SPLIT_ESB_MAX_PAYLOAD` | 48 | max on-air bytes (>= largest split msg) |
| `ZMK_SPLIT_ESB_RX_QUEUE_SIZE` | 16 | RX packet queue depth |
| `ZMK_SPLIT_ESB_REPLY_QUEUE_SIZE` | 8 | central reverse-channel queue depth |
| `ZMK_SPLIT_ESB_TX_FIFO_SIZE` | 8 | radio TX FIFO depth |
| `ZMK_SPLIT_ESB_RX_FIFO_SIZE` | 8 | radio RX FIFO depth |
| `ZMK_SPLIT_ESB_PRIORITY` | 50 | transport registration priority |

## Limitations

`ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT` is 0 on an ESB-only central (ZMK derives it
from BLE/wired counts). Events still deliver, but:

- Commands to the peripheral need `BEHAVIOR_LOCALITY_EVENT_SOURCE`, source = its id.
- No `BEHAVIOR_LOCALITY_GLOBAL` broadcast or HID-indicator forwarding (both index that count).
