// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Compact on-air encoding for split peripheral events.
 * One event becomes a 1-byte type tag plus only that type's union payload, dropping
 * the 3-byte gap from ZMK's unpacked event-type enum and the unused union tail (the
 * sensor member is widest, motion only needs input_event).
 * A coalesced 2-axis motion packet shrinks from 34 to 20 bytes on air.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zmk/split/transport/types.h>

/* Largest encoded event: tag plus the widest union member. */
#define ESB_WIRE_MAX_EVENT_SIZE                                                                    \
    (1 + sizeof(((struct zmk_split_transport_peripheral_event *)0)->data))

/* Append one encoded event to out (out_cap bytes free).
 * Returns bytes written (1 + payload), or 0 if the event does not fit. */
size_t esb_wire_encode_event(uint8_t *out, size_t out_cap,
                             const struct zmk_split_transport_peripheral_event *event);

/* Decode one event from in (avail bytes available).
 * Writes *event, returns bytes consumed, or 0 if the tag is unknown or the payload
 * is truncated. */
size_t esb_wire_decode_event(const uint8_t *in, size_t avail,
                             struct zmk_split_transport_peripheral_event *event);
