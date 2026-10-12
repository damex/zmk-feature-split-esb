// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Peripheral traffic on a one millisecond tick into a real central through the fake ACK FIFO.
 * A case defines peripheral_traffic_mode.
 * Exits 1 on the first write the central's ACK FIFO refuses, 0 after the whole run.
 */
#pragma once

#include <stdbool.h>

struct peripheral_traffic_mode {
    bool left_types;
    bool mouse_moves;
    bool mouse_lossy;
};

extern const struct peripheral_traffic_mode peripheral_traffic_mode;
