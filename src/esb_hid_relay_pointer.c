// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay, central half: pointer reports out to relay-role pipes. */
#define DT_DRV_COMPAT zmk_input_processor_esb_relay_pointer

#include "esb_hid_relay_pointer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>
#include <zmk/hid.h>
#include <zmk/pointing.h>

#include "esb_hid_relay_central.h"

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "one relay pointer processor serves every input listener");
BUILD_ASSERT(sizeof(struct zmk_hid_mouse_report) == ESB_HID_RELAY_POINTER_LENGTH,
             "relay pointer length drifted from ZMK's mouse report");
BUILD_ASSERT(offsetof(struct zmk_hid_mouse_report, body.buttons) ==
                 ESB_HID_RELAY_POINTER_BUTTONS_OFFSET,
             "relay pointer buttons drifted from ZMK's mouse report");
BUILD_ASSERT(offsetof(struct zmk_hid_mouse_report, body.d_x) == ESB_HID_RELAY_POINTER_MOTION_OFFSET,
             "relay pointer motion drifted from ZMK's mouse report");
BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report) + sizeof(struct zmk_hid_consumer_report) +
                     ESB_HID_RELAY_POINTER_LENGTH <=
                 CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "keyboard, consumer and pointer reports must fit one relay reply");

static atomic_t sum_d_x;
static atomic_t sum_d_y;
static atomic_t sum_d_scroll_y;
static atomic_t sum_d_scroll_x;
static atomic_t buttons;
static atomic_t buttons_changed;
static atomic_t refresh_due;

static void add_motion(uint16_t code, int32_t value) {
    switch (code) {
    case INPUT_REL_X:
        (void)atomic_add(&sum_d_x, value);
        break;
    case INPUT_REL_Y:
        (void)atomic_add(&sum_d_y, value);
        break;
    case INPUT_REL_WHEEL:
        (void)atomic_add(&sum_d_scroll_y, value);
        break;
    case INPUT_REL_HWHEEL:
        (void)atomic_add(&sum_d_scroll_x, value);
        break;
    default:
        /* ZMK's input listener ignores other relative codes too */
        break;
    }
}

static bool button_bit(uint16_t code, zmk_mouse_button_flags_t *bit) {
    if (code == INPUT_BTN_TOUCH) {
        *bit = (zmk_mouse_button_flags_t)BIT(0);
        return true;
    }
    if (code < INPUT_BTN_0 || code >= INPUT_BTN_0 + ZMK_HID_MOUSE_NUM_BUTTONS) {
        return false;
    }
    *bit = (zmk_mouse_button_flags_t)BIT(code - INPUT_BTN_0);
    return true;
}

static atomic_val_t set_button(zmk_mouse_button_flags_t bit, bool pressed) {
    if (pressed) {
        return atomic_or(&buttons, bit);
    }
    return atomic_and(&buttons, ~(atomic_val_t)bit);
}

static void add_button(uint16_t code, int32_t value) {
    zmk_mouse_button_flags_t bit = 0;
    if (!button_bit(code, &bit)) {
        return;
    }
    bool pressed = value != 0;
    bool was_pressed = (set_button(bit, pressed) & bit) != 0;
    if (was_pressed != pressed) {
        atomic_set(&buttons_changed, 1);
    }
}

static void stage_buttons(void) {
    struct zmk_hid_mouse_report report = {
        .report_id = ZMK_HID_REPORT_ID_MOUSE,
        .body = {.buttons = (zmk_mouse_button_flags_t)atomic_get(&buttons)},
    };
    esb_hid_relay_stage(&report, sizeof(report));
}

static int relay_pointer_handle_event(const struct device *dev, struct input_event *event,
                                      uint32_t param1, uint32_t param2,
                                      struct zmk_input_processor_state *state) {
    ARG_UNUSED(dev);
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);
    if (!esb_hid_relay_active()) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    if (event->type == INPUT_EV_REL) {
        add_motion(event->code, event->value);
    } else if (event->type == INPUT_EV_KEY) {
        add_button(event->code, event->value);
    }
    /* One button report per sync, as ZMK's own listener sends. */
    if (event->sync && atomic_cas(&buttons_changed, 1, 0)) {
        stage_buttons();
    }
    return ZMK_INPUT_PROC_CONTINUE;
}

static int16_t take_delta(atomic_t *sum) {
    return (int16_t)CLAMP(atomic_get(sum), INT16_MIN, INT16_MAX);
}

static bool body_moves(const struct zmk_hid_mouse_report_body *body) {
    return body->d_x != 0 || body->d_y != 0 || body->d_scroll_y != 0 || body->d_scroll_x != 0;
}

void esb_hid_relay_pointer_refresh(void) {
    atomic_set(&refresh_due, 1);
}

size_t esb_hid_relay_pointer_take(uint8_t *out, size_t room) {
    if (room < ESB_HID_RELAY_POINTER_LENGTH) {
        return 0;
    }
    struct zmk_hid_mouse_report report = {.report_id = ZMK_HID_REPORT_ID_MOUSE};
    /* Paused, every report releases the buttons the dongle still holds. */
    if (esb_hid_relay_active()) {
        report.body = (struct zmk_hid_mouse_report_body){
            .buttons = (zmk_mouse_button_flags_t)atomic_get(&buttons),
            .d_x = take_delta(&sum_d_x),
            .d_y = take_delta(&sum_d_y),
            .d_scroll_y = take_delta(&sum_d_scroll_y),
            .d_scroll_x = take_delta(&sum_d_scroll_x),
        };
    }
    if (!body_moves(&report.body) && atomic_get(&refresh_due) == 0) {
        return 0;
    }
    memcpy(out, &report, sizeof(report));
    return sizeof(report);
}

void esb_hid_relay_pointer_sent(const uint8_t *bytes) {
    struct zmk_hid_mouse_report report = {0};
    memcpy(&report, bytes, sizeof(report));
    (void)atomic_sub(&sum_d_x, report.body.d_x);
    (void)atomic_sub(&sum_d_y, report.body.d_y);
    (void)atomic_sub(&sum_d_scroll_y, report.body.d_scroll_y);
    (void)atomic_sub(&sum_d_scroll_x, report.body.d_scroll_x);
    (void)atomic_clear(&refresh_due);
}

void esb_hid_relay_pointer_reset(void) {
    (void)atomic_clear(&sum_d_x);
    (void)atomic_clear(&sum_d_y);
    (void)atomic_clear(&sum_d_scroll_y);
    (void)atomic_clear(&sum_d_scroll_x);
    zmk_mouse_button_flags_t held = 0;
    if (esb_hid_relay_active()) {
        held = zmk_hid_get_mouse_report()->body.buttons;
    }
    (void)atomic_set(&buttons, held);
}

static const struct zmk_input_processor_driver_api relay_pointer_api = {
    .handle_event = relay_pointer_handle_event,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                      &relay_pointer_api);
