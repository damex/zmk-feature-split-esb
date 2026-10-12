// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Scenario on a real central.c: a peripheral encoder's keepalive total jumps past the resync cap.
 * Exits 0 once the jump adopts a new baseline without replay and the next step arrives.
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
#define GAP_DEG 230
#define STEP_DEG 15
#define STEP_UDEG (STEP_DEG * ESB_SENSOR_MICRODEG_PER_DEG)

BUILD_ASSERT(GAP_DEG * ESB_SENSOR_MICRODEG_PER_DEG > ESB_SENSOR_RESYNC_UDEG,
             "gap passes the resync cap");

const uint8_t scenario_pipe = DT_PROP(DT_NODELABEL(left), pipe);

const struct scenario_event scenario_expected[] = {
    {.kind = SCENARIO_EVENT_SENSOR, .id = SENSOR_INDEX, .value = STEP_UDEG},
};
const size_t scenario_expected_count = ARRAY_SIZE(scenario_expected);

static const int32_t baseline_totals_deg[] = {BASELINE_DEG};
static const int32_t jumped_totals_deg[] = {BASELINE_DEG + GAP_DEG};
static const int32_t stepped_totals_deg[] = {BASELINE_DEG + GAP_DEG + STEP_DEG};

void scenario(void) {
    const struct scenario_keepalive baseline = {
        .sensor_totals_deg = baseline_totals_deg,
        .sensor_count = ARRAY_SIZE(baseline_totals_deg),
    };
    const struct scenario_keepalive jumped = {
        .sensor_totals_deg = jumped_totals_deg,
        .sensor_count = ARRAY_SIZE(jumped_totals_deg),
    };
    const struct scenario_keepalive stepped = {
        .sensor_totals_deg = stepped_totals_deg,
        .sensor_count = ARRAY_SIZE(stepped_totals_deg),
    };
    scenario_send_keepalive(&baseline);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_keepalive(&jumped);
    k_msleep(SCENARIO_STEP_MS);
    scenario_send_keepalive(&stepped);
}
