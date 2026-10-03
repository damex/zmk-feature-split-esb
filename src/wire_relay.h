// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Wire relay on a split peripheral. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(CONFIG_ZMK_SPLIT_ESB_WIRE_RELAY)
bool wire_relay_owns_pipe(uint8_t pipe);
void wire_relay_forward_to_peer(const uint8_t *data, size_t length);
#else
static inline bool wire_relay_owns_pipe(uint8_t pipe) {
    (void)pipe;
    return false;
}
static inline void wire_relay_forward_to_peer(const uint8_t *data, size_t length) {
    (void)data;
    (void)length;
}
#endif
