// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* First report after a silence past link loss detection sets a new baseline. */
#define DT_DRV_COMPAT zmk_split_esb

#include <stddef.h>

#include <zephyr/sys/util.h>

#include "dongle_pointer.h"
#include "hop_internal.h"

#define BASE_X 100
#define BASE_Y (-7)
#define RESTART_X 20
#define RESTART_Y 9
#define MOTION_X 5
#define SILENCE_MS (ESB_HOP_LOSS_DETECT_MS + 1)

const struct dongle_pointer_step dongle_pointer_steps[] = {
    {.total_x = BASE_X, .total_y = BASE_Y},
    {.total_x = RESTART_X, .total_y = RESTART_Y, .silence_before_ms = SILENCE_MS},
    {
        .total_x = RESTART_X + MOTION_X,
        .total_y = RESTART_Y,
        .reaches_sink = true,
        .skippable = true,
        .d_x = MOTION_X,
    },
};
const size_t dongle_pointer_step_count = ARRAY_SIZE(dongle_pointer_steps);
