// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Scenario harness on a real central.c, with ESB link core and hop fakes. */

#include "scenario.h"

#include <stdlib.h>

#include <zephyr/drivers/sensor.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/split/transport/types.h>

#include <esb.h>

#include "central.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "esb_sensor_sync.h"
#include "esb_wire.h"
#include "hop.h"

#define SCENARIO_START_MS 50
#define SCENARIO_SETTLE_MS 100
#define SCENARIO_THREAD_STACK_SIZE 2048
#define SCENARIO_THREAD_PRIORITY 7
#define SCENARIO_HELD_MAX 4
#define SCENARIO_SENSOR_MAX 2

static atomic_t heard;
static atomic_t silent;
static atomic_t observed;

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    ARG_UNUSED(callback);
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    return 0;
}

int esb_link_hfclk_acquire(void) {
    return 0;
}

void hop_boot_mask(void) {
}

uint32_t hop_pipe_quiet_ms(uint8_t pipe) {
    ARG_UNUSED(pipe);
    if (atomic_get(&silent) != 0) {
        return UINT32_MAX;
    }
    return 0;
}

bool hop_pipe_heard(uint8_t pipe) {
    if (pipe != scenario_pipe) {
        return false;
    }
    return atomic_get(&heard) != 0;
}

void scenario_observe(enum scenario_event_kind kind, uint32_t id, int64_t value) {
    size_t index = (size_t)atomic_inc(&observed);
    if (index >= scenario_expected_count) {
        printk("FAIL: extra event kind %d id %u value %lld\n",
               (int)kind,
               (unsigned int)id,
               (long long)value);
        exit(1);
    }
    const struct scenario_event *want = &scenario_expected[index];
    if (want->kind != kind || want->id != id || want->value != value) {
        printk("FAIL: event %u of %u, expected kind %d id %u value %lld, "
               "got kind %d id %u value %lld\n",
               (unsigned int)(index + 1),
               (unsigned int)scenario_expected_count,
               (int)want->kind,
               (unsigned int)want->id,
               (long long)want->value,
               (int)kind,
               (unsigned int)id,
               (long long)value);
        exit(1);
    }
}

static int observation_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    if (position != NULL) {
        if (position->source == scenario_pipe) {
            int64_t state = SCENARIO_RELEASED;
            if (position->state) {
                state = SCENARIO_PRESSED;
            }
            scenario_observe(SCENARIO_EVENT_POSITION, position->position, state);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_sensor_event *sensor = as_zmk_sensor_event(event);
    if (sensor != NULL) {
        struct sensor_value value = sensor->channel_data[0].value;
        scenario_observe(SCENARIO_EVENT_SENSOR,
                         sensor->sensor_index,
                         esb_sensor_udeg(value.val1, value.val2));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(scenario, observation_listener);
ZMK_SUBSCRIPTION(scenario, zmk_position_state_changed);
ZMK_SUBSCRIPTION(scenario, zmk_sensor_event);

static void ingest(const uint8_t *data, size_t length) {
    atomic_set(&heard, 1);
    central_ingest_packet(scenario_pipe, data, length);
}

static void send_event(const struct zmk_split_transport_peripheral_event *event) {
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    size_t length = esb_wire_encode_event(wire, sizeof(wire), event);
    if (length == 0) {
        printk("FAIL: scenario event does not encode\n");
        exit(1);
    }
    ingest(wire, length);
}

void scenario_send_key(uint32_t position, bool pressed) {
    const struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = (uint8_t)position, .pressed = pressed},
    };
    send_event(&event);
}

void scenario_send_sensor_total(uint8_t sensor_index, int32_t total_deg) {
    const struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT,
        .data.sensor_event = {
            .channel_data = {.value = {.val1 = total_deg}, .channel = SENSOR_CHAN_ROTATION},
            .sensor_index = sensor_index,
        },
    };
    send_event(&event);
}

void scenario_send_input_key(uint8_t reg, uint16_t code, bool pressed) {
    const struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data.input_event = {.reg = reg,
                             .type = INPUT_EV_KEY,
                             .code = code,
                             .value = pressed,
                             .sync = true},
    };
    send_event(&event);
}

void scenario_send_keepalive(const struct scenario_keepalive *keepalive) {
    uint8_t bitmap[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    for (uint8_t index = 0; index < keepalive->pressed_count; index++) {
        esb_keepalive_bitmap_set(bitmap, keepalive->pressed_positions[index], true);
    }
    if (keepalive->held_count > SCENARIO_HELD_MAX) {
        printk("FAIL: scenario keepalive holds more than %d keys\n", SCENARIO_HELD_MAX);
        exit(1);
    }
    if (keepalive->sensor_count > SCENARIO_SENSOR_MAX) {
        printk("FAIL: scenario keepalive carries more than %d sensors\n", SCENARIO_SENSOR_MAX);
        exit(1);
    }
    int64_t totals_udeg[SCENARIO_SENSOR_MAX] = {0};
    for (uint8_t index = 0; index < keepalive->sensor_count; index++) {
        totals_udeg[index] = esb_sensor_udeg(keepalive->sensor_totals_deg[index], 0);
    }
    const struct esb_keepalive_snapshot snapshot = {
        .state = ESB_KEEPALIVE_IDLE,
        .battery_level = keepalive->battery_level,
        .position_bitmap = bitmap,
        .held_keys = keepalive->held_keys,
        .held_count = keepalive->held_count,
        .sensor_totals_udeg = totals_udeg,
        .sensor_count = keepalive->sensor_count,
    };
    uint8_t encoded[ESB_KEEPALIVE_LENGTH(SCENARIO_HELD_MAX, 0, SCENARIO_SENSOR_MAX)];
    size_t length = esb_keepalive_encode(encoded, sizeof(encoded), &snapshot);
    if (length == 0) {
        printk("FAIL: scenario keepalive does not encode\n");
        exit(1);
    }
    ingest(encoded, length);
}

void scenario_go_silent(void) {
    atomic_set(&silent, 1);
}

void scenario_go_live(void) {
    atomic_set(&silent, 0);
}

static void scenario_thread_fn(void *unused_a, void *unused_b, void *unused_c) {
    ARG_UNUSED(unused_a);
    ARG_UNUSED(unused_b);
    ARG_UNUSED(unused_c);
    scenario();
    k_msleep(SCENARIO_SETTLE_MS);
    size_t seen = (size_t)atomic_get(&observed);
    if (seen != scenario_expected_count) {
        printk("FAIL: %u of %u events\n",
               (unsigned int)seen,
               (unsigned int)scenario_expected_count);
        exit(1);
    }
    printk("PASS: all %u events in order, nothing extra\n", (unsigned int)seen);
    exit(0);
}

K_THREAD_DEFINE(scenario_thread,
                SCENARIO_THREAD_STACK_SIZE,
                scenario_thread_fn,
                NULL,
                NULL,
                NULL,
                SCENARIO_THREAD_PRIORITY,
                0,
                SCENARIO_START_MS);
