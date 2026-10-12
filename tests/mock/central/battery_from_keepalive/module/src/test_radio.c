// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Scenario on a real central.c: keepalives report battery unknown, then one level twice.
 * Exits 0 once the first known level raises one battery event and its repeat raises none.
 * Exits 1 on a wrong, missing or extra event.
 */
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#include "esb_keepalive.h"
#include "scenario.h"

#define PIPE DT_PROP(DT_NODELABEL(left), pipe)
#define BATTERY_LEVEL 87

const uint8_t scenario_pipe = PIPE;

const struct scenario_event scenario_expected[] = {
    {.kind = SCENARIO_EVENT_BATTERY, .id = PIPE, .value = BATTERY_LEVEL},
};
const size_t scenario_expected_count = ARRAY_SIZE(scenario_expected);

static int battery_listener(const zmk_event_t *event) {
    const struct zmk_peripheral_battery_state_changed *battery =
        as_zmk_peripheral_battery_state_changed(event);
    if (battery != NULL) {
        scenario_observe(SCENARIO_EVENT_BATTERY, battery->source, battery->state_of_charge);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(battery_from_keepalive, battery_listener);
ZMK_SUBSCRIPTION(battery_from_keepalive, zmk_peripheral_battery_state_changed);

void scenario(void) {
    const struct scenario_keepalive unknown = {.battery_level = ESB_KEEPALIVE_BATTERY_UNKNOWN};
    const struct scenario_keepalive known = {.battery_level = BATTERY_LEVEL};
    scenario_send_keepalive(&unknown);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_keepalive(&known);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_keepalive(&known);
}
