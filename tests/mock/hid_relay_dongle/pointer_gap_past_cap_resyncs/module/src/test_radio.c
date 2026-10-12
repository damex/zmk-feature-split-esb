// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Jump past the resync cap sends nothing, the next report arrives as the delta from it. */
#include <stddef.h>

#include <zephyr/sys/util.h>

#include "dongle_pointer.h"
#include "esb_motion_total.h"

#define BASE_X 100
#define BASE_Y (-7)
#define FAR_X (BASE_X + ESB_MOTION_RESYNC_COUNTS + 1)
#define MOTION_X 5

const struct dongle_pointer_step dongle_pointer_steps[] = {
    {.total_x = BASE_X, .total_y = BASE_Y},
    {.total_x = FAR_X, .total_y = BASE_Y},
    {
        .total_x = FAR_X + MOTION_X,
        .total_y = BASE_Y,
        .reaches_sink = true,
        .skippable = true,
        .d_x = MOTION_X,
    },
};
const size_t dongle_pointer_step_count = ARRAY_SIZE(dongle_pointer_steps);
