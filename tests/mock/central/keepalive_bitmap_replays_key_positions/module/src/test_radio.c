// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Scenario on a real central.c: a peripheral key's press and release are both lost.
 * Exits 0 once a keepalive holding the key replays the press and an empty one the release.
 * Exits 1 on a wrong, missing or extra event.
 */
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "scenario.h"

#define POSITION 3

const uint8_t scenario_pipe = DT_PROP(DT_NODELABEL(left), pipe);

const struct scenario_event scenario_expected[] = {
    {.kind = SCENARIO_EVENT_POSITION, .id = POSITION, .value = SCENARIO_PRESSED},
    {.kind = SCENARIO_EVENT_POSITION, .id = POSITION, .value = SCENARIO_RELEASED},
};
const size_t scenario_expected_count = ARRAY_SIZE(scenario_expected);

static const uint32_t held_positions[] = {POSITION};

void scenario(void) {
    const struct scenario_keepalive holding = {
        .pressed_positions = held_positions,
        .pressed_count = ARRAY_SIZE(held_positions),
    };
    const struct scenario_keepalive released = {0};
    scenario_send_keepalive(&holding);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_keepalive(&released);
}
