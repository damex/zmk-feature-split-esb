# Architecture

Paths a packet takes between peripherals and the dongle. Names match the source.
Brackets are code, right edge names the file. Dashed rules split execution
contexts: radio ISR only moves bytes, rx_thread dispatches, system workqueue is
the only place that touches ZMK behavior state.

## Boot

Both chains run in the main thread through SYS_INIT, before any tick fires.
ZMK transport `set_enabled` can fire earlier, so hop work items sit statically
initialized.

```
+-- central boot --------------------------------------------------------------+
|                                                                              |
|  [reply_queue_init]                          esb_link_central.c              |
|  POST_KERNEL: k_msgq_init per pipe                                           |
|      |                                                                       |
|      v                                                                       |
|  [central_init]                              central.c                       |
|  battery cache to unknown, staleness tick scheduled                          |
|      |                                                                       |
|      v                                                                       |
|  [settings_load_subtree("esb_hop")] --> [hop_mask_settings_set]              |
|                                         persisted mask captured              |
|      |                                                                       |
|      v                                                                       |
|  [esb_link_hfclk_acquire]                    esb_link.c                      |
|  onoff request, spins until HFXO runs                                        |
|      |                                                                       |
|      v                                                                       |
|  [hop_boot_mask]                             hop_central.c                   |
|      |                                                                       |
|      |--> [ensure_mask] full pool, anchors resolved                          |
|      |--> [esb_survey_run]                   esb_survey.c                    |
|      |    radio irq disarmed, power cycled,                                  |
|      |    RXEN + RSSI sweep per channel, power cycled again                  |
|      |--> [hop_policy_survey_mask]           hop_policy.c                    |
|      |    persisted + busy channels masked, min-active floor kept            |
|      |--> [epoch bump + hop_index pick]                                      |
|      v                                                                       |
|  [esb_link_init]                             esb_link.c                      |
|      |                                                                       |
|      |--> [rx_thread created]                                                |
|      |--> [esb_init]                         NCS esb.c                       |
|      |    radio, PPI, IRQ configured from reset state                        |
|      |--> [esb_link_radio_setup]                                             |
|      |    esb_set_address_length / base_address / prefixes /                 |
|      |    rf_channel(hop_current_channel) / tx_power                         |
|      |--> [esb_link_role_start] --> [esb_start_rx]                           |
|      v                                                                       |
|  [hop_start] --> [decision_work due in idle-keepalive-ms]                    |
+------------------------------------------------------------------------------+

+-- peripheral boot -----------------------------------------------------------+
|                                                                              |
|  [peripheral_init]                           peripheral.c                    |
|      |                                                                       |
|      v                                                                       |
|  [hop_restore]                               hop_peripheral.c                |
|  retained RAM magic + checksum checked,                                      |
|  active_mask / adopted_epoch / hop_index resumed                             |
|      |                                                                       |
|      v                                                                       |
|  [esb_link_init]                             esb_link.c                      |
|      |                                                                       |
|      |--> [rx_thread created]                                                |
|      |--> [esb_link_hfclk_acquire]                                           |
|      |--> [esb_init]                         NCS esb.c                       |
|      |--> [esb_link_radio_setup]                                             |
|      |    rf_channel tunes to the resumed hop_current_channel                |
|      |--> [esb_link_role_start] no-op on PTX                                 |
|      v                                                                       |
|  [hop_start] --> [keepalive_work due in hop-window-ms]                       |
|      |                                                                       |
|      v                                                                       |
|  [esb_config_init]                           esb_config.c                    |
|  settings_load_subtree("esb") applies saved                                  |
|  tx_power / retransmit count + delay live                                    |
|      |                                                                       |
|      v                                                                       |
|  [first keepalive tick]                                                      |
|  polls the central on the resumed channel                                    |
+------------------------------------------------------------------------------+
```

## Uplink: peripheral event to central

```
+-- peripheral ----------------------------------------------------------------+
| caller thread: input thread for pointer events,                              |
| system workqueue for the rest                                                |
|                                                                              |
|   [ZMK event]                                                                |
|       |                                                                      |
|       v                                                                      |
|   [peripheral_report_event]                       peripheral.c               |
|       |                                                                      |
|       |--key----> [pressed bitmap]    state taps, the keepalive              |
|       |--sensor-> [running total]     carries and heals them                 |
|       |                                                                      |
|       |input only                                                            |
|       v                                                                      |
|   [esb_batch]                                     esb_batch.c                |
|   coalesce to sync flag or full batch                                        |
|       |                                                                      |
|       v         key/sensor/battery skip the batch                            |
|   [esb_wire_encode_event]                         esb_wire.c                 |
|       |                                                                      |
|       v                                                                      |
|   [esb_link_send]                       esb_link_peripheral.c                |
|   noack for lossy-codes input,                                               |
|   keepalives enter here from the tick                                        |
|       |                                                                      |
|       v                                                                      |
|   [ESB TX FIFO]                                                              |
| - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -|
| RADIO ISR                                                                    |
|                                                                              |
|   [ESB TX FIFO] --> [ESB PTX radio]                                          |
|   [TX events] --> [link_acked, max_tx_attempts]                              |
|                   feed the keepalive tick                                    |
+---------------|--------------------------------------------------------------+
                | 2.4 GHz
+-- dongle -----|--------------------------------------------------------------+
| RADIO ISR     v                                                              |
|                                                                              |
|   [ESB PRX radio] --> [drain RX FIFO]             esb_link.c                 |
|       |                                                                      |
|       v                                                                      |
|   [hop_consume_rx]                                hop_central.c              |
|   stamp heard + rssi, motion/active bits                                     |
|       |                                                                      |
|       v                                                                      |
|   [SPSC ring]                                                                |
| - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -|
| rx_thread                                                                    |
|                                                                              |
|   [SPSC ring] --> [central_on_rx: decode]         central.c                  |
|       |input                     |key/battery/sensor/keepalive               |
|       v                          v                                           |
|   [input subsystem]          [event msgq]                                    |
|   direct, input_report is                                                    |
|   safe from any context                                                      |
| - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -|
| system workqueue                                                             |
|                                                                              |
|   [event msgq] --> [reconcile]                                               |
|                    orphan drop, lost-release heal,                           |
|                    battery, sensor deltas                                    |
|                        |                                                     |
|                        v                                                     |
|                    [ZMK central handler]                                     |
+------------------------------------------------------------------------------+
```

## Reverse channel: central to peripheral, rides ACK payloads

```
+-- dongle --------------------------------------------------------------------+
| system workqueue                                                             |
|                                                                              |
|   [ZMK command]        [decision tick]  [hid listener]                       |
|       |                     |                |                               |
|       v                     +-------+--------+                               |
|   [central_send_command]            v                                        |
|       |                 [beacon / mask update]    hop_central.c              |
|       v                 epoch, hid state,                                    |
|   [reply queue pipe N]  peer battery + rssi                                  |
|   k_msgq, backlog                   |                                        |
|                                     v                                        |
|                         [control latch pipe N]  esb_link_central.c           |
|                         latest value wins                                    |
| - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -|
| RADIO ISR                                                                    |
|                                                                              |
|   [RX from pipe N] --> [fill ACK FIFO]                                       |
|   control latch first, else one queued reply                                 |
|                     |                                                        |
+---------------------|--------------------------------------------------------+
                      | ACK rides peripheral's next transmit
+-- peripheral -------|--------------------------------------------------------+
| RADIO ISR           v                                                        |
|                                                                              |
|   [ACK payload] --> [hop_consume_rx]              hop_peripheral.c           |
|       |               |beacon         |mask update                           |
|       |               v               v                                      |
|       |           [epoch, hid,    [staged mask]                              |
|       |            peer roster]   adopted on epoch swap                      |
|       |command                                                               |
|       v                                                                      |
|   [SPSC ring]                                                                |
| - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -|
| rx_thread                                                                    |
|                                                                              |
|   [SPSC ring] --> [peripheral_on_rx] --> [command msgq]                      |
| - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -|
| system workqueue                                                             |
|                                                                              |
|   [command msgq] --> [behavior invocation]                                   |
|                                                                              |
|   [zmk_split_esb_peer_battery / peer_rssi / hid_*]                           |
|   read the stored beacon fields, feed the widgets                            |
+------------------------------------------------------------------------------+
```

## Ticks

All three run on the system workqueue.

```
+-- peripheral keepalive tick ---- hop-window-ms active, ----------------------+
|                                  idle-keepalive-ms idle                      |
|                                                                              |
|  [link_acked, max_tx_attempts]               hop_peripheral.c                |
|                |                                                             |
|                v                                                             |
|  [keepalive_work_fn]                                                         |
|   |beacon epoch changed    |acked               |silent                      |
|   v                        v                    v                            |
|  [adopt_epoch]        [connected_window]   [lost_window]                     |
|  retune to epoch      attempts EWMA ->     degrade undo ->                   |
|  channel, mask swap   retransmit budget,   sweep the pool ->                 |
|                       degrade step         camp the anchors                  |
|   |                        |                    |                            |
|   +------------+-----------+--------------------+                            |
|                v                                                             |
|  [send keepalive] --> [retained RAM]                                         |
|  state, key bitmap,     last-acked channel,                                  |
|  battery, cost, totals  epoch, mask                                          |
+------------------------------------------------------------------------------+

+-- central decision tick -------- idle-keepalive-ms window -------------------+
|                                                                              |
|  [heard / motion / active / link cost]        hop_central.c                  |
|      |                                                                       |
|      v                                                                       |
|  [per-pipe loss + channel score] --> [mask recompute]                        |
|      |                               retest ladder,                          |
|      v                               debounced settings save                 |
|  [hop vote]                                                                  |
|      |                                                                       |
|      v                                                                       |
|  [epoch hop: retune, mask commits]                                           |
|                                                                              |
|  [anchor dip] --> [rejoin + mask beacons for lost pipes]                     |
+------------------------------------------------------------------------------+

+-- central staleness tick ------- 500 ms -------------------------------------+
|                                                                              |
|  [quiet pipe past peripheral-timeout-ms]      central.c                      |
|      |                                                                       |
|      v                                                                       |
|  [release held state]                                                        |
|  keys, input reset, sensor baselines, connection events                      |
+------------------------------------------------------------------------------+
```

## Rendezvous: reacquiring a lost link

Time flows down, one column per device.

```
+-- lost link -----------------------------------------------------------------+
|                                                                              |
|  peripheral                          central                                 |
|                                                                              |
|  [TX unacked]                        [pipe quiet grows]                      |
|  link_acked drops                    hop_pipe_quiet_ms past                  |
|      |                               ESB_HOP_LOSS_DETECT_MS                  |
|      v                                   |                                   |
|  [degrade undo]                          v                                   |
|  only after a failed degrade         [needs_rendezvous]                      |
|  step: one step back first               |                                   |
|      |                                   v                                   |
|      v                               [anchor dip]                            |
|  [sweep the pool]                    one window on the next                  |
|  index_next each dwell,              anchor every dip period,                |
|  ESB_HOP_SWEEP_WINDOWS long          rejoin beacon latched                   |
|      |                                   |                                   |
|      v                                   |                                   |
|  [camp the anchors]                      |                                   |
|  hop_policy_camp_step, dwell             |                                   |
|  outlasts a full dip cycle               |                                   |
|      |                                   |                                   |
|      +------ keepalive lands on ---------+                                   |
|             the dipped anchor                                                |
|                     |                                                        |
|                     v                                                        |
|  [ACK carries rejoin beacon + staged mask update]                            |
|      |                                   |                                   |
|      v                                   v                                   |
|  [adopt_epoch]                       [pipe heard]                            |
|  staged mask swapped in,             quiet resets, dips stop.                |
|  retune to the live channel          held state survives when                |
|                                      rendezvous beats                        |
|                                      peripheral-timeout-ms                   |
|      |                                                                       |
|      v                                                                       |
|  [connected: keepalives ack on the live channel]                             |
+------------------------------------------------------------------------------+
```

## Power

```
+-- peripheral power ----------------------------------------------------------+
|                                                                              |
|  [ZMK activity] --> [esb_sleep listener]          esb_sleep.c                |
|   |active            |idle              |sleep                               |
|   v                  v                  v                                    |
|  [hold HFXO]     [release HFXO]     [radio stopped]                          |
|  sends start     after a hold       before SYSTEM_OFF                        |
|  instantly       window, next                                                |
|                  send re-locks                                               |
+------------------------------------------------------------------------------+
```

## Packets

First byte routes a packet. Control tags sit at 0xFD..0xFF. Event type tags
are a handful of small integers, build-asserted below the keepalive tag.

```
event packet     peripheral to central, events back to back in one payload
  [0]    event type
  [1..]  payload, input packed to 9 bytes:
         reg u8, sync u8, type u8, code u16 le, value i32 le
         other types copy their ZMK struct verbatim

keepalive        peripheral to central, every tick
  [0]     0xFF
  [1]     state, 0 idle / 1 active
  [2..9]  pressed-position bitmap, 64 keys
  [10]    battery percent, 0xFF unknown
  [11]    uplink link cost, attempts EWMA x10
  [12..]  per-sensor running total, i64 le microdegrees each

beacon           central to peripheral, rides an ACK
  [0]    0xFE
  [1]    epoch
  [2]    hid modifiers
  [3]    hid indicators
  [4..]  per-pipe roster: battery u8, rssi i8 dbm

mask update      central to peripheral, rides an ACK
  [0]    0xFD
  [1..]  active-channel bitmap, one bit per hop-channels entry

command          central to peripheral, rides an ACK
  raw struct zmk_split_transport_central_command, routed by exact length
```

## Backpressure

Where a full queue costs something, and what it costs. Depth symbols carry
the `CONFIG_ZMK_SPLIT_ESB` prefix.

| queue | depth | when full |
|---|---|---|
| RX SPSC ring | `..._RX_QUEUE_SIZE` | ISR drops the packet |
| event msgq, central | `..._EVENT_QUEUE_SIZE` | dropped, keepalive heals keys |
| command msgq, peripheral | `..._COMMAND_QUEUE_SIZE` | command dropped |
| reply queue, per pipe | `reply-queue-depth` | stage returns -ENOBUFS |
| control latch, per pipe | one slot per kind | newest overwrites, by design |
| ESB TX FIFO | `CONFIG_ESB_TX_FIFO_SIZE` | send fails, stall flush recovers |
