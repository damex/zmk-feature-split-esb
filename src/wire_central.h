// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(CONFIG_ZMK_SPLIT_ESB_WIRE_PEER)
bool wire_central_owns_pipe(uint8_t pipe);
int wire_central_send_command(const uint8_t *data, size_t length);
bool wire_central_peer_is_up(void);
#else
static inline bool wire_central_owns_pipe(uint8_t pipe) {
    (void)pipe;
    return false;
}
static inline int wire_central_send_command(const uint8_t *data, size_t length) {
    (void)data;
    (void)length;
    return -ENOSYS;
}
static inline bool wire_central_peer_is_up(void) {
    return false;
}
#endif
