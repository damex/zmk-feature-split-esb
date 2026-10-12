// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Report repeating the delivered totals and buttons sends nothing. */
#include <stddef.h>

#include <zephyr/sys/util.h>

#include "dongle_pointer.h"

#define BASE_X 100
#define BASE_Y (-7)
#define MOTION_X 5
#define MOVED_X (BASE_X + MOTION_X)

const struct dongle_pointer_step dongle_pointer_steps[] = {
    {.total_x = BASE_X, .total_y = BASE_Y},
    {
        .total_x = MOVED_X,
        .total_y = BASE_Y,
        .reaches_sink = true,
        .skippable = true,
        .d_x = MOTION_X,
    },
    {.total_x = MOVED_X, .total_y = BASE_Y},
};
const size_t dongle_pointer_step_count = ARRAY_SIZE(dongle_pointer_steps);
