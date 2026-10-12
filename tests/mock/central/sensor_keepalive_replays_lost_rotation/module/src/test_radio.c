// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Scenario on a real central.c: a peripheral encoder's rotation event is lost.
 * Exits 0 once the next keepalive total replays the lost step after the live one.
 * Exits 1 on a wrong, missing or extra event.
 */
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "esb_sensor_sync.h"
#include "scenario.h"

#define SENSOR_INDEX 0
#define BASELINE_DEG 100
#define STEP_DEG 15
#define STEP_UDEG (STEP_DEG * ESB_SENSOR_MICRODEG_PER_DEG)

const uint8_t scenario_pipe = DT_PROP(DT_NODELABEL(left), pipe);

const struct scenario_event scenario_expected[] = {
    {.kind = SCENARIO_EVENT_SENSOR, .id = SENSOR_INDEX, .value = STEP_UDEG},
    {.kind = SCENARIO_EVENT_SENSOR, .id = SENSOR_INDEX, .value = STEP_UDEG},
};
const size_t scenario_expected_count = ARRAY_SIZE(scenario_expected);

static const int32_t baseline_totals_deg[] = {BASELINE_DEG};
static const int32_t replay_totals_deg[] = {BASELINE_DEG + 2 * STEP_DEG};

void scenario(void) {
    const struct scenario_keepalive baseline = {
        .sensor_totals_deg = baseline_totals_deg,
        .sensor_count = ARRAY_SIZE(baseline_totals_deg),
    };
    const struct scenario_keepalive replay = {
        .sensor_totals_deg = replay_totals_deg,
        .sensor_count = ARRAY_SIZE(replay_totals_deg),
    };
    scenario_send_keepalive(&baseline);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_sensor_total(SENSOR_INDEX, BASELINE_DEG + STEP_DEG);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_keepalive(&replay);
}
