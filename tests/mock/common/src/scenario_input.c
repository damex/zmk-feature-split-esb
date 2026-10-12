// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Scenario input observer: input key events from any input device into the check. */

#include "scenario.h"

#include <stddef.h>

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/util.h>

static void input_observer(struct input_event *event, void *user_data) {
    ARG_UNUSED(user_data);
    if (event->type == INPUT_EV_KEY) {
        scenario_observe(SCENARIO_EVENT_INPUT_KEY, event->code, event->value);
    }
}

INPUT_CALLBACK_DEFINE(NULL, input_observer, NULL);
