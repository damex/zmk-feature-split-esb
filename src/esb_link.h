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

/* Central: no-op.
 * Peripheral: idle gates HFXO. */
void esb_link_set_idle(bool idle);

#if !defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Peripheral only.
 * Send one packet. ack == false is fire-and-forget: no ACK, so no reverse-channel
 * reply rides it. */
int esb_link_send(const uint8_t *data, size_t length, bool ack);

/* Peripheral only.
 * Send the acked keepalive, this device's state snapshot. */
void esb_link_send_keepalive(uint8_t state);

/* Peripheral only.
 * Returns the encoded length, 0 when out_size is too small.
 * Defined in peripheral.c. */
uint8_t esb_link_keepalive_fill(uint8_t *out, size_t out_size, uint8_t state);

/* Peripheral only.
 * Radio-ISR context.
 * Defined in peripheral.c. */
void esb_link_hid_state_store(uint8_t modifiers, uint8_t indicators);
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

uint8_t esb_central_battery_level(uint8_t pipe);

#define ESB_LINK_CONTROL_MAX_LENGTH 16

enum esb_link_control {
    ESB_LINK_CONTROL_BEACON,
    ESB_LINK_CONTROL_MASK,
    ESB_LINK_CONTROL_COUNT,
};

/* Central only. */
int esb_link_latch_control(uint8_t pipe, enum esb_link_control kind, const uint8_t *data,
                           size_t length);
#endif
