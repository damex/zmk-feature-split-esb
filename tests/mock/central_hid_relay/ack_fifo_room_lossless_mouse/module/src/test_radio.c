// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Lossless mouse streams motion with ACK while the left half types with shift. */
#include "peripheral_traffic.h"

const struct peripheral_traffic_mode peripheral_traffic_mode = {
    .left_types = true,
    .mouse_moves = true,
    .mouse_lossy = false,
};
