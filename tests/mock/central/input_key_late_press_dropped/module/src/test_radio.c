// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Scenario on a real central.c: a keepalive holding a peripheral input key overtakes its press.
 * Exits 0 once the keepalive presses the key, the late press drops and the release arrives.
 * Exits 1 on a wrong, missing or extra event.
 */
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "esb_keepalive.h"
#include "scenario.h"

#define INPUT_REG DT_REG_ADDR(DT_NODELABEL(split_input))

const uint8_t scenario_pipe = DT_PROP(DT_NODELABEL(left), pipe);

const struct scenario_event scenario_expected[] = {
    {.kind = SCENARIO_EVENT_INPUT_KEY, .id = INPUT_BTN_1, .value = SCENARIO_PRESSED},
    {.kind = SCENARIO_EVENT_INPUT_KEY, .id = INPUT_BTN_1, .value = SCENARIO_RELEASED},
};
const size_t scenario_expected_count = ARRAY_SIZE(scenario_expected);

static const struct esb_keepalive_held_key held_keys[] = {
    {.reg = INPUT_REG, .code = INPUT_BTN_1},
};

void scenario(void) {
    const struct scenario_keepalive holding = {
        .held_keys = held_keys,
        .held_count = ARRAY_SIZE(held_keys),
    };
    scenario_send_keepalive(&holding);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_input_key(INPUT_REG, INPUT_BTN_1, true);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_input_key(INPUT_REG, INPUT_BTN_1, false);
}
