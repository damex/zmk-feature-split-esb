// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Lossless mouse moves every millisecond, the dongle polls every millisecond. */
#include "peripheral_traffic.h"

const struct peripheral_traffic_mode peripheral_traffic_mode = {
    .mouse_moves = true,
    .mouse_lossy = false,
    .relay_new_motion_every_poll = true,
};
