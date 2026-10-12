// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* No relay dongle, lossy mouse streams motion without ACK while the left half types with shift. */
#include "peripheral_traffic.h"

const struct peripheral_traffic_mode peripheral_traffic_mode = {
    .left_types = true,
    .mouse_moves = true,
    .mouse_lossy = true,
};
