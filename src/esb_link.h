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
 *   - central: an event transmitted by the peripheral.
 *   - peripheral: a command the central rode back on an ACK.
 */
typedef void (*esb_link_rx_callback_t)(const uint8_t *data, size_t length);

/* Initialize the radio for this device's role; the central also starts listening. */
int esb_link_init(esb_link_rx_callback_t callback);

int esb_link_set_enabled(bool enabled);

#if !defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Peripheral only. Send one packet. ack == false is fire-and-forget: no ACK, so no
 * reverse-channel reply rides it. */
int esb_link_send(const uint8_t *data, size_t length, bool ack);
#endif

#if defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Central only. Queue one packet to ride the next ACK back to the peripheral.
 * Returns -ENOBUFS if the reply queue is full. */
int esb_link_stage_reply(const uint8_t *data, size_t length);
#endif
