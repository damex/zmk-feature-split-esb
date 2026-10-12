// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Left half and mouse idle, only keepalives and relay polls. */
#include "peripheral_traffic.h"

const struct peripheral_traffic_mode peripheral_traffic_mode = {
    .left_types = false,
    .mouse_moves = false,
    .mouse_lossy = false,
};
