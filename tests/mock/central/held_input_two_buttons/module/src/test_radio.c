// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central, one peripheral holding two buttons on one input register.
 * Release of the first button is lost, the next keepalive holds only the second.
 * Exits 0 once the input split device saw both presses, the first button released alone,
 * then the second released by a keepalive holding nothing.
 * Exits 1 on a wrong or extra event or at deadline.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/types.h>

#include <esb.h>

#include "central.h"
#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "esb_wire.h"
#include "hop.h"

#define PIPE DT_PROP(DT_NODELABEL(left), pipe)
#define INPUT_REG DT_REG_ADDR(DT_NODELABEL(split_input))
#define SCRIPT_START_MS 50
#define STEP_MS 20
#define SETTLE_MS 100
#define VERDICT_DEADLINE_MS 1000
#define RELEASED 0
#define PRESSED 1

struct button_event {
    uint16_t code;
    int32_t value;
};

static const struct button_event expected[] = {
    {.code = INPUT_BTN_0, .value = PRESSED},
    {.code = INPUT_BTN_1, .value = PRESSED},
    {.code = INPUT_BTN_0, .value = RELEASED},
    {.code = INPUT_BTN_1, .value = RELEASED},
};

enum step_kind {
    STEP_PRESS,
    STEP_KEEPALIVE,
};

struct script_step {
    enum step_kind kind;
    uint16_t code;
};

static const struct script_step script[] = {
    {.kind = STEP_PRESS, .code = INPUT_BTN_0},
    {.kind = STEP_PRESS, .code = INPUT_BTN_1},
    /* Release of button 0 lost, the keepalive holds button 1 only. */
    {.kind = STEP_KEEPALIVE, .code = INPUT_BTN_1},
    {.kind = STEP_KEEPALIVE},
};

static size_t next_step;
static size_t next_expected;

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
    return 0;
}

bool hop_pipe_heard(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return false;
}

static void pass_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("PASS: both buttons pressed, each released on its own\n");
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(pass_work, pass_fn);

static void input_observer(struct input_event *event, void *user_data) {
    ARG_UNUSED(user_data);
    if (event->type != INPUT_EV_KEY) {
        return;
    }
    if (next_expected == ARRAY_SIZE(expected)) {
        printk("FAIL: extra button code 0x%03x value %d\n", event->code, (int)event->value);
        exit(1);
    }
    const struct button_event *want = &expected[next_expected];
    if (event->code != want->code || event->value != want->value) {
        printk("FAIL: event %u of %u, expected code 0x%03x value %d, got code 0x%03x value %d\n",
               (unsigned int)(next_expected + 1),
               (unsigned int)ARRAY_SIZE(expected),
               want->code,
               (int)want->value,
               event->code,
               (int)event->value);
        exit(1);
    }
    next_expected++;
    if (next_expected == ARRAY_SIZE(expected)) {
        k_work_reschedule(&pass_work, K_MSEC(SETTLE_MS));
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(split_input)), input_observer, NULL);

static void send_press(const struct script_step *step) {
    const struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data.input_event = {.reg = INPUT_REG,
                             .type = INPUT_EV_KEY,
                             .code = step->code,
                             .value = PRESSED,
                             .sync = true},
    };
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    size_t length = esb_wire_encode_event(wire, sizeof(wire), &event);
    if (length == 0) {
        printk("FAIL: script press does not encode\n");
        exit(1);
    }
    central_ingest_packet(PIPE, wire, length);
}

static void send_keepalive(const struct script_step *step) {
    static const uint8_t no_positions[ESB_KEEPALIVE_BITMAP_BYTES];
    const struct esb_keepalive_held_key held = {.reg = INPUT_REG, .code = step->code};
    uint8_t held_count = 0;
    if (step->code != 0) {
        held_count = 1;
    }
    const struct esb_keepalive_snapshot snapshot = {
        .state = ESB_KEEPALIVE_IDLE,
        .battery_level = ESB_KEEPALIVE_BATTERY_UNKNOWN,
        .position_bitmap = no_positions,
        .held_keys = &held,
        .held_count = held_count,
    };
    uint8_t keepalive[ESB_KEEPALIVE_LENGTH(1, 0, 0)];
    size_t length = esb_keepalive_encode(keepalive, sizeof(keepalive), &snapshot);
    if (length == 0) {
        printk("FAIL: script keepalive does not encode\n");
        exit(1);
    }
    central_ingest_packet(PIPE, keepalive, length);
}

static void script_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(script_work, script_fn);

static void script_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct script_step *step = &script[next_step];
    switch (step->kind) {
    case STEP_PRESS:
        send_press(step);
        break;
    case STEP_KEEPALIVE:
        send_keepalive(step);
        break;
    default:
        printk("FAIL: unknown script step %d\n", (int)step->kind);
        exit(1);
    }
    next_step++;
    if (next_step < ARRAY_SIZE(script)) {
        k_work_reschedule(&script_work, K_MSEC(STEP_MS));
    }
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: %u of %u button events by deadline\n",
           (unsigned int)next_expected,
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
