// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <zephyr/kernel.h>

#include <zmk/event_manager.h>

struct zmk_split_esb_peripheral_changed {
    uint8_t source;
    bool connected;
};

ZMK_EVENT_DECLARE(zmk_split_esb_peripheral_changed);
