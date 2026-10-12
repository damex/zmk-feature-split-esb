// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Dongle pointer steps into a real HID relay peripheral: reports carrying running totals.
 * A case defines the steps and what each one sends to the sink.
 * Exits 1 on a missing, extra or wrong sink report, 0 after the last step.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dongle_pointer_step {
    /* Sent report */
    int16_t total_x;
    int16_t total_y;
    uint8_t buttons;
    uint32_t silence_before_ms;

    /* Sink */
    bool sink_busy;
    bool reaches_sink;
    bool skippable;
    int16_t d_x;
    int16_t d_y;
};

extern const struct dongle_pointer_step dongle_pointer_steps[];
extern const size_t dongle_pointer_step_count;
