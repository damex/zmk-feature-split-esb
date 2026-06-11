// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Single-device ESB radio layer: whole packets in and out, no framing or
 * reassembly (ESB delivers discrete CRC-checked packets).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One received packet, handed up in thread context (not the radio ISR).
 * Central: event from the peripheral on pipe.
 * Peripheral: command rode back on an ACK, pipe is its own. */
typedef void (*esb_link_rx_callback_t)(uint8_t pipe, const uint8_t *data, size_t length);

/* Initialize radio for this device's role.
 * Central: PRX, starts listening.
 * Peripheral: PTX, idle until first send. */
int esb_link_init(esb_link_rx_callback_t callback);

/* Central: re-enable restarts RX.
 * Peripheral: re-enable restarts hop work only. */
int esb_link_set_enabled(bool enabled);

#if !defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Peripheral only.
 * Send one packet. ack == false is fire-and-forget: no ACK, so no reverse-channel
 * reply rides it. */
int esb_link_send(const uint8_t *data, size_t length, bool ack);

/* Peripheral only.
 * Send the acked keepalive, this device's state snapshot: hop-engine state byte,
 * pressed-position bitmap, battery level. */
void esb_link_send_keepalive(uint8_t state, const uint8_t *position_bitmap,
                             uint8_t battery_level);

/* Peripheral only.
 * Role layer's live pressed-position bitmap (ESB_KEEPALIVE_BITMAP_BYTES), defined in
 * peripheral.c and read by the keepalive tick. */
const uint8_t *esb_link_keepalive_bitmap(void);

/* Peripheral only.
 * Role layer's battery level for keepalives, ESB_KEEPALIVE_BATTERY_UNKNOWN when the
 * device does not report battery. Defined in peripheral.c. */
uint8_t esb_link_keepalive_battery_level(void);
#endif

#if defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Central only.
 * Fill out_ids with the peripheral source ids (= pipe numbers).
 * Returns the count. */
uint8_t esb_link_source_ids(uint8_t *out_ids);

/* Central only.
 * Queue one packet to ride peripheral `pipe`'s next ACK back to it.
 * Returns -ENOBUFS if the reply queue is full. */
int esb_link_stage_reply(uint8_t pipe, const uint8_t *data, size_t length);
#endif
