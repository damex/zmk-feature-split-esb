// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Peripheral traffic on a one millisecond tick into a real central through the fake ACK FIFO.
 * A case defines peripheral_traffic_mode.
 * Exits 1 on the first write the central's ACK FIFO refuses, 0 after the whole run.
 * With relay_new_motion_every_poll, also exits 1 on a relay poll whose ACK carries no new
 * motion once motion arrived. A poll answered with link control counts apart.
 */
#pragma once

#include <stdbool.h>

struct peripheral_traffic_mode {
    bool left_types;
    bool mouse_moves;
    bool mouse_lossy;
    bool relay_new_motion_every_poll;
};

extern const struct peripheral_traffic_mode peripheral_traffic_mode;
