// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Input split on a split peripheral. */
#pragma once

#include <stdint.h>

#include <zmk/split/transport/types.h>

#include "esb_keepalive.h"

#if defined(CONFIG_ZMK_INPUT_SPLIT)
#define PERIPHERAL_INPUT_HELD_KEYS_MAX CONFIG_ZMK_INPUT_SPLIT_MAX_TRACKED_KEYS

/* Any event type, input events only from the input thread. */
void peripheral_input_note_event(const struct zmk_split_transport_peripheral_event *event);

/* out holds PERIPHERAL_INPUT_HELD_KEYS_MAX entries. */
uint8_t peripheral_input_held_keys(struct esb_keepalive_held_key *out);
#else
#define PERIPHERAL_INPUT_HELD_KEYS_MAX 0

static inline void
peripheral_input_note_event(const struct zmk_split_transport_peripheral_event *event) {
    (void)event;
}

static inline uint8_t peripheral_input_held_keys(struct esb_keepalive_held_key *out) {
    (void)out;
    return 0;
}
#endif
