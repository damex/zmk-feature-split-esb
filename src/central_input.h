// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Input split on a split central. */
#pragma once

#include <stdint.h>

#include <zmk/split/transport/central.h>
#include <zmk/split/transport/types.h>

#if defined(CONFIG_ZMK_INPUT_SPLIT)
#define CENTRAL_INPUT_HELD_KEYS_MAX CONFIG_ZMK_INPUT_SPLIT_MAX_TRACKED_KEYS

/* RX context per pipe: ESB RX thread, wire RX on the wire peer pipe.
 * Reconcile takes a keepalive that passed esb_keepalive_matches. */
void central_input_deliver_event(const struct zmk_split_transport_central *transport, uint8_t pipe,
                                 const struct zmk_split_transport_peripheral_event *event);
void central_input_reconcile(const struct zmk_split_transport_central *transport, uint8_t pipe,
                             const uint8_t *keepalive);

/* Staleness tick only. */
void central_input_release_pipe(uint8_t pipe);
#else
#define CENTRAL_INPUT_HELD_KEYS_MAX 0

static inline void
central_input_deliver_event(const struct zmk_split_transport_central *transport, uint8_t pipe,
                            const struct zmk_split_transport_peripheral_event *event) {
    (void)zmk_split_transport_central_peripheral_event_handler(transport, pipe, *event);
}

static inline void central_input_reconcile(const struct zmk_split_transport_central *transport,
                                           uint8_t pipe, const uint8_t *keepalive) {
    (void)transport;
    (void)pipe;
    (void)keepalive;
}

static inline void central_input_release_pipe(uint8_t pipe) {
    (void)pipe;
}
#endif
