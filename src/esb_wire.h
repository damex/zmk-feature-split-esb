// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Compact on-air encoding for split peripheral events.
 * One event becomes a 1-byte type tag plus a tight payload. The input event (the
 * high-rate path) is field-packed to drop ZMK's struct padding, 13 bytes to 10, so a
 * coalesced 2-axis motion packet shrinks from 26 to 20 bytes on air.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zmk/split/transport/types.h>

#define ESB_WIRE_MAX_EVENT_SIZE                                                                    \
    (1 + sizeof(((struct zmk_split_transport_peripheral_event *)0)->data))

/* Encoded size of one input event (tag plus packed fields).
 * The peripheral coalesces only input events, so this bounds how many fit in a packet.
 * Pinned to the codec layout by a build assert in esb_wire.c. */
#define ESB_WIRE_INPUT_EVENT_SIZE 10

/* Append one encoded event to out (out_cap bytes free).
 * Returns bytes written (1 + payload), or 0 if the event does not fit. */
size_t esb_wire_encode_event(uint8_t *out, size_t out_cap,
                             const struct zmk_split_transport_peripheral_event *event);

/* Decode one event from in (avail bytes available).
 * Writes *event, returns bytes consumed, or 0 if the tag is unknown or the payload
 * is truncated. */
size_t esb_wire_decode_event(const uint8_t *in, size_t avail,
                             struct zmk_split_transport_peripheral_event *event);
