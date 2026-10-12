// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Motion while a delivered button stays held arrives skippable. */
#include <stddef.h>

#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/pointing.h>

#include "dongle_pointer.h"

#define BASE_X 100
#define BASE_Y (-7)
#define MOTION_X 5

const struct dongle_pointer_step dongle_pointer_steps[] = {
    {.total_x = BASE_X, .total_y = BASE_Y},
    {.total_x = BASE_X, .total_y = BASE_Y, .buttons = MB1, .reaches_sink = true},
    {
        .total_x = BASE_X + MOTION_X,
        .total_y = BASE_Y,
        .buttons = MB1,
        .reaches_sink = true,
        .skippable = true,
        .d_x = MOTION_X,
    },
};
const size_t dongle_pointer_step_count = ARRAY_SIZE(dongle_pointer_steps);
