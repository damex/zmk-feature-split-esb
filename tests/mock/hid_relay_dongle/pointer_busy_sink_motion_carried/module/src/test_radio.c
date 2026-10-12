// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Motion the busy sink refused arrives with the next report. */
#include <stddef.h>

#include <zephyr/sys/util.h>

#include "dongle_pointer.h"

#define BASE_X 100
#define BASE_Y (-7)
#define REFUSED_X 4
#define REFUSED_Y 3
#define MOTION_X 2

const struct dongle_pointer_step dongle_pointer_steps[] = {
    {.total_x = BASE_X, .total_y = BASE_Y},
    {
        .total_x = BASE_X + REFUSED_X,
        .total_y = BASE_Y + REFUSED_Y,
        .sink_busy = true,
        .skippable = true,
    },
    {
        .total_x = BASE_X + REFUSED_X + MOTION_X,
        .total_y = BASE_Y + REFUSED_Y,
        .reaches_sink = true,
        .skippable = true,
        .d_x = REFUSED_X + MOTION_X,
        .d_y = REFUSED_Y,
    },
};
const size_t dongle_pointer_step_count = ARRAY_SIZE(dongle_pointer_steps);
