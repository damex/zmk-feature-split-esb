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

/*
 * One received packet, handed up in a thread context (not the radio ISR):
 *   - central: an event from the peripheral on `pipe`.
 *   - peripheral: a command the central rode back on an ACK (`pipe` is its own).
 */
typedef void (*esb_link_rx_callback_t)(uint8_t pipe, const uint8_t *data, size_t length);

/* Initialize the radio for this device's role.
 * The central also starts listening. */
int esb_link_init(esb_link_rx_callback_t callback);

int esb_link_set_enabled(bool enabled);

/* Diagnostic count of received packets dropped because the RX ring was full. */
uint32_t esb_link_rx_dropped(void);

#if !defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Peripheral only.
 * Send one packet. ack == false is fire-and-forget: no ACK, so no reverse-channel
 * reply rides it. */
int esb_link_send(const uint8_t *data, size_t length, bool ack);

/* Peripheral only.
 * Send a length-1 acked keepalive on this device's pipe, for the hop engine. */
void esb_link_send_keepalive(void);
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
