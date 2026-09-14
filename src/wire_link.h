// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*wire_link_rx_callback_t)(const uint8_t *payload, size_t length, void *user_data);

struct wire_link_subscription {
    wire_link_rx_callback_t callback;
    void *user_data;
};

int wire_link_register_rx(struct wire_link_subscription *subscription);
int wire_link_send_event(const uint8_t *payload, size_t length);
int wire_link_send_keepalive(const uint8_t *payload, size_t length);
bool wire_link_is_up(void);
