// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Scenario on a real central.c: a peripheral key's release is lost, then the key is pressed again.
 * Exits 0 once the repeated press arrives as a release and a press, then the real release.
 * Exits 1 on a wrong, missing or extra event.
 */
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "scenario.h"

#define POSITION 1

const uint8_t scenario_pipe = DT_PROP(DT_NODELABEL(left), pipe);

const struct scenario_event scenario_expected[] = {
    {.kind = SCENARIO_EVENT_POSITION, .id = POSITION, .value = SCENARIO_PRESSED},
    {.kind = SCENARIO_EVENT_POSITION, .id = POSITION, .value = SCENARIO_RELEASED},
    {.kind = SCENARIO_EVENT_POSITION, .id = POSITION, .value = SCENARIO_PRESSED},
    {.kind = SCENARIO_EVENT_POSITION, .id = POSITION, .value = SCENARIO_RELEASED},
};
const size_t scenario_expected_count = ARRAY_SIZE(scenario_expected);

void scenario(void) {
    scenario_send_key(POSITION, true);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_key(POSITION, true);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_key(POSITION, false);
}
