// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a central, scripting one peripheral.
 * Exits 0 once lost, stray and silent-peripheral traffic reconciles into the expected events.
 * Exits 1 on a wrong or extra event or at deadline.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/sensors.h>
#include <zmk/split/transport/types.h>

#include <esb.h>

#include "central.h"
#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "esb_sensor_sync.h"
#include "esb_wire.h"
#include "hop.h"

#define PIPE DT_PROP(DT_NODELABEL(left), pipe)
#define INPUT_REG DT_REG_ADDR(DT_NODELABEL(split_input))
#define SCRIPT_START_MS 50
#define STEP_MS 20
#define SETTLE_MS 100
#define STALE_WAIT_MS 1000 /* outlasts one staleness tick */
#define VERDICT_DEADLINE_MS 3000
#define SENSOR_INDEX 0
#define BATTERY_LEVEL 87
#define STEP_UDEG (15 * ESB_SENSOR_MICRODEG_PER_DEG)
#define RELEASED 0
#define PRESSED 1

enum observation_kind {
    OBSERVED_POSITION,
    OBSERVED_SENSOR,
    OBSERVED_BATTERY,
    OBSERVED_INPUT,
};

struct observation {
    enum observation_kind kind;
    uint32_t id;
    int64_t value;
};

static const struct observation expected[] = {
    {.kind = OBSERVED_POSITION, .id = 1, .value = PRESSED},
    {.kind = OBSERVED_POSITION, .id = 1, .value = RELEASED},
    {.kind = OBSERVED_POSITION, .id = 1, .value = PRESSED},
    {.kind = OBSERVED_POSITION, .id = 1, .value = RELEASED},
    {.kind = OBSERVED_POSITION, .id = 3, .value = PRESSED},
    {.kind = OBSERVED_POSITION, .id = 3, .value = RELEASED},
    {.kind = OBSERVED_SENSOR, .id = SENSOR_INDEX, .value = STEP_UDEG},
    {.kind = OBSERVED_SENSOR, .id = SENSOR_INDEX, .value = STEP_UDEG},
    {.kind = OBSERVED_SENSOR, .id = SENSOR_INDEX, .value = STEP_UDEG},
    {.kind = OBSERVED_BATTERY, .id = PIPE, .value = BATTERY_LEVEL},
    {.kind = OBSERVED_INPUT, .id = INPUT_BTN_0, .value = PRESSED},
    {.kind = OBSERVED_INPUT, .id = INPUT_BTN_0, .value = RELEASED},
    {.kind = OBSERVED_INPUT, .id = INPUT_BTN_1, .value = PRESSED},
    {.kind = OBSERVED_INPUT, .id = INPUT_BTN_1, .value = RELEASED},
    {.kind = OBSERVED_INPUT, .id = INPUT_BTN_0, .value = PRESSED},
    {.kind = OBSERVED_POSITION, .id = 0, .value = PRESSED},
    {.kind = OBSERVED_POSITION, .id = 0, .value = RELEASED},
    {.kind = OBSERVED_INPUT, .id = INPUT_BTN_0, .value = RELEASED},
    {.kind = OBSERVED_INPUT, .id = INPUT_BTN_0, .value = PRESSED},
};

enum step_kind {
    STEP_KEY,
    STEP_KEEPALIVE,
    STEP_SENSOR,
    STEP_INPUT,
    STEP_GO_SILENT,
    STEP_GO_LIVE,
};

struct script_step {
    enum step_kind kind;
    uint32_t position;
    bool pressed;
    uint8_t battery;
    int32_t total_deg;
    uint16_t input_code;
    uint16_t held_codes[2];
};

static const struct script_step script[] = {
    {.kind = STEP_KEY, .position = 1, .pressed = true},
    /* Release of position 1 lost, the repeated press heals it. */
    {.kind = STEP_KEY, .position = 1, .pressed = true},
    {.kind = STEP_KEY, .position = 1, .pressed = false},
    /* Press of position 2 lost, its release is an orphan. */
    {.kind = STEP_KEY, .position = 2, .pressed = false},
    /* Press of position 3 lost, the keepalive bitmap replays it. */
    {.kind = STEP_KEEPALIVE, .position = 3, .pressed = true,
     .battery = ESB_KEEPALIVE_BATTERY_UNKNOWN, .total_deg = 100},
    {.kind = STEP_KEEPALIVE, .battery = ESB_KEEPALIVE_BATTERY_UNKNOWN, .total_deg = 100},
    {.kind = STEP_SENSOR, .total_deg = 115},
    /* Rotation to 130 lost, the keepalive total replays it. */
    {.kind = STEP_KEEPALIVE, .battery = ESB_KEEPALIVE_BATTERY_UNKNOWN, .total_deg = 130},
    /* Gap past the resync cap adopts the total without replay. */
    {.kind = STEP_KEEPALIVE, .battery = ESB_KEEPALIVE_BATTERY_UNKNOWN, .total_deg = 330},
    {.kind = STEP_KEEPALIVE, .battery = ESB_KEEPALIVE_BATTERY_UNKNOWN, .total_deg = 345},
    {.kind = STEP_KEEPALIVE, .battery = BATTERY_LEVEL, .total_deg = 345},
    {.kind = STEP_KEEPALIVE, .battery = BATTERY_LEVEL, .total_deg = 345},
    {.kind = STEP_INPUT, .input_code = INPUT_BTN_0, .pressed = true},
    /* Release of button 0 lost, the keepalive without it releases it. */
    {.kind = STEP_KEEPALIVE, .battery = BATTERY_LEVEL, .total_deg = 345},
    /* Press of button 1 lost, the keepalive holding it presses it. */
    /* Button 2 past the one tracked key never presses, not even on repeat. */
    {.kind = STEP_KEEPALIVE, .battery = BATTERY_LEVEL, .total_deg = 345,
     .held_codes = {INPUT_BTN_1, INPUT_BTN_2}},
    {.kind = STEP_KEEPALIVE, .battery = BATTERY_LEVEL, .total_deg = 345,
     .held_codes = {INPUT_BTN_1, INPUT_BTN_2}},
    /* Keepalive raced ahead of the press, the late press drops. */
    {.kind = STEP_INPUT, .input_code = INPUT_BTN_1, .pressed = true},
    {.kind = STEP_INPUT, .input_code = INPUT_BTN_1, .pressed = false},
    /* Button 0 held through silence, released stale, re-pressed by the next keepalive. */
    {.kind = STEP_INPUT, .input_code = INPUT_BTN_0, .pressed = true},
    {.kind = STEP_KEY, .position = 0, .pressed = true},
    {.kind = STEP_GO_SILENT},
    {.kind = STEP_GO_LIVE},
    {.kind = STEP_KEEPALIVE, .battery = BATTERY_LEVEL, .total_deg = 345,
     .held_codes = {INPUT_BTN_0}},
};

static size_t next_step;
static size_t next_expected;
static bool peripheral_silent;

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
    if (peripheral_silent) {
        return UINT32_MAX;
    }
    return 0;
}

bool hop_pipe_heard(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return peripheral_silent;
}

static void pass_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("PASS: all %u reconcile events in order, nothing extra\n",
           (unsigned int)ARRAY_SIZE(expected));
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(pass_work, pass_fn);

static void observe(enum observation_kind kind, uint32_t id, int64_t value) {
    if (next_expected == ARRAY_SIZE(expected)) {
        printk("FAIL: extra event kind %d id %u value %lld\n", (int)kind, (unsigned int)id,
               (long long)value);
        exit(1);
    }
    const struct observation *want = &expected[next_expected];
    if (want->kind != kind || want->id != id || want->value != value) {
        printk("FAIL: event %u of %u, expected kind %d id %u value %lld, got kind %d id %u "
               "value %lld\n",
               (unsigned int)(next_expected + 1), (unsigned int)ARRAY_SIZE(expected),
               (int)want->kind, (unsigned int)want->id, (long long)want->value, (int)kind,
               (unsigned int)id, (long long)value);
        exit(1);
    }
    next_expected++;
    if (next_expected == ARRAY_SIZE(expected)) {
        k_work_reschedule(&pass_work, K_MSEC(SETTLE_MS));
    }
}

static int observation_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    if (position != NULL) {
        if (position->source == PIPE) {
            observe(OBSERVED_POSITION, position->position, position->state ? PRESSED : RELEASED);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_sensor_event *sensor = as_zmk_sensor_event(event);
    if (sensor != NULL) {
        struct sensor_value value = sensor->channel_data[0].value;
        observe(OBSERVED_SENSOR, sensor->sensor_index, esb_sensor_udeg(value.val1, value.val2));
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_peripheral_battery_state_changed *battery =
        as_zmk_peripheral_battery_state_changed(event);
    if (battery != NULL) {
        observe(OBSERVED_BATTERY, battery->source, battery->state_of_charge);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(central_reconcile_test, observation_listener);
ZMK_SUBSCRIPTION(central_reconcile_test, zmk_position_state_changed);
ZMK_SUBSCRIPTION(central_reconcile_test, zmk_sensor_event);
ZMK_SUBSCRIPTION(central_reconcile_test, zmk_peripheral_battery_state_changed);

static void input_observer(struct input_event *event, void *user_data) {
    ARG_UNUSED(user_data);
    if (event->type == INPUT_EV_KEY) {
        observe(OBSERVED_INPUT, event->code, event->value);
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(split_input)), input_observer, NULL);

static void send_event(const struct zmk_split_transport_peripheral_event *event) {
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    size_t length = esb_wire_encode_event(wire, sizeof(wire), event);
    if (length == 0) {
        printk("FAIL: script event does not encode\n");
        exit(1);
    }
    central_ingest_packet(PIPE, wire, length);
}

static void send_key(const struct script_step *step) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = (uint8_t)step->position, .pressed = step->pressed},
    };
    send_event(&event);
}

static void send_sensor_total(const struct script_step *step) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT,
        .data.sensor_event = {
            .channel_data = {.value = {.val1 = step->total_deg}, .channel = SENSOR_CHAN_ROTATION},
            .sensor_index = SENSOR_INDEX,
        },
    };
    send_event(&event);
}

static void send_input(const struct script_step *step) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data.input_event = {.reg = INPUT_REG, .type = INPUT_EV_KEY, .code = step->input_code,
                             .value = step->pressed, .sync = true},
    };
    send_event(&event);
}

static void send_keepalive(const struct script_step *step) {
    uint8_t bitmap[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    if (step->pressed) {
        esb_keepalive_bitmap_set(bitmap, step->position, true);
    }
    int64_t totals[ZMK_KEYMAP_SENSORS_LEN] = {0};
    totals[SENSOR_INDEX] = esb_sensor_udeg(step->total_deg, 0);
    struct esb_keepalive_held_key held[ARRAY_SIZE(step->held_codes)] = {0};
    uint8_t held_count = 0;
    for (size_t index = 0; index < ARRAY_SIZE(step->held_codes); index++) {
        if (step->held_codes[index] != 0) {
            held[held_count] = (struct esb_keepalive_held_key){
                .reg = INPUT_REG,
                .code = step->held_codes[index],
            };
            held_count++;
        }
    }
    const struct esb_keepalive_snapshot snapshot = {
        .state = ESB_KEEPALIVE_IDLE,
        .battery_level = step->battery,
        .position_bitmap = bitmap,
        .held_keys = held,
        .held_count = held_count,
        .sensor_totals_udeg = totals,
        .sensor_count = ZMK_KEYMAP_SENSORS_LEN,
    };
    uint8_t keepalive[ESB_KEEPALIVE_LENGTH(ARRAY_SIZE(held), ZMK_KEYMAP_SENSORS_LEN)];
    size_t length = esb_keepalive_encode(keepalive, sizeof(keepalive), &snapshot);
    if (length == 0) {
        printk("FAIL: script keepalive does not encode\n");
        exit(1);
    }
    central_ingest_packet(PIPE, keepalive, length);
}

static void run_step(const struct script_step *step) {
    switch (step->kind) {
    case STEP_KEY:
        send_key(step);
        break;
    case STEP_KEEPALIVE:
        send_keepalive(step);
        break;
    case STEP_SENSOR:
        send_sensor_total(step);
        break;
    case STEP_INPUT:
        send_input(step);
        break;
    case STEP_GO_SILENT:
        peripheral_silent = true;
        break;
    case STEP_GO_LIVE:
        peripheral_silent = false;
        break;
    default:
        printk("FAIL: unknown script step %d\n", (int)step->kind);
        exit(1);
    }
}

static void script_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(script_work, script_fn);

static uint32_t step_gap_ms(const struct script_step *step) {
    if (step->kind == STEP_GO_SILENT) {
        return STALE_WAIT_MS;
    }
    return STEP_MS;
}

static void script_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct script_step *step = &script[next_step];
    run_step(step);
    next_step++;
    if (next_step < ARRAY_SIZE(script)) {
        k_work_reschedule(&script_work, K_MSEC(step_gap_ms(step)));
    }
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: reconcile stopped at event %u of %u\n", (unsigned int)(next_expected + 1),
           (unsigned int)ARRAY_SIZE(expected));
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&script_work, K_MSEC(SCRIPT_START_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
