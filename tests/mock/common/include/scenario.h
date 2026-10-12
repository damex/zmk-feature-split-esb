// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Scenario harness on a real central.c: a fake peripheral sends, the central's events
 * are checked in order.
 * A case defines scenario(), scenario_pipe and the expected events.
 * Input key cases link the scenario input observer.
 * Battery cases feed scenario_observe from their own listener.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esb_keepalive.h"

#define SCENARIO_RELEASED 0
#define SCENARIO_PRESSED 1
#define SCENARIO_STEP_MS 20
#define SCENARIO_STALE_WAIT_MS 1000 /* outlasts one staleness tick */

enum scenario_event_kind {
    SCENARIO_EVENT_POSITION,
    SCENARIO_EVENT_SENSOR,
    SCENARIO_EVENT_BATTERY,
    SCENARIO_EVENT_INPUT_KEY,
};

struct scenario_event {
    enum scenario_event_kind kind;
    uint32_t id;
    int64_t value;
};

struct scenario_keepalive {
    const uint32_t *pressed_positions;
    uint8_t pressed_count;
    const struct esb_keepalive_held_key *held_keys;
    uint8_t held_count;
    const int32_t *sensor_totals_deg;
    uint8_t sensor_count;
    uint8_t battery_level;
};

extern const uint8_t scenario_pipe;
extern const struct scenario_event scenario_expected[];
extern const size_t scenario_expected_count;

/* Runs on the scenario thread, sleeping between steps is fine. */
void scenario(void);

void scenario_send_key(uint32_t position, bool pressed);
void scenario_send_sensor_total(uint8_t sensor_index, int32_t total_deg);
void scenario_send_input_key(uint8_t reg, uint16_t code, bool pressed);

void scenario_send_keepalive(const struct scenario_keepalive *keepalive);

/* Silent: heard before, quiet past every timeout. */
void scenario_go_silent(void);
void scenario_go_live(void);

/* Exits 1 when the event is not the next expected one. */
void scenario_observe(enum scenario_event_kind kind, uint32_t id, int64_t value);
