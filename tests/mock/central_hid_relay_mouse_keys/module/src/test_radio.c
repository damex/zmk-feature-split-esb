// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central with a relay dongle and ZMK's mouse keys.
 * Relay pointer processor sits on the mouse key press, move and scroll listeners.
 * Keymap clicks two buttons across relay polls, taps one inside a single poll,
 * then moves and scrolls.
 * Exits 0 once the relay ACKs carry every button change in order and every move and scroll count.
 * Exits 1 on an unknown report, two pointer reports with motion in one ACK, a lost button change
 * or a count that differs from what the mouse keys reported.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/pointing.h>
#include <zmk/hid.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define REPORT_ID_OFFSET 0
#define POLL_START_MS 20
#define VERDICT_MS 1000
#define BUTTON_CHANGES_MAX 16

struct pointer_totals {
    int32_t d_x;
    int32_t d_y;
    int32_t d_scroll_y;
    int32_t d_scroll_x;
};

static const uint8_t expected_buttons[] = {
    LCLK, LCLK | RCLK, RCLK, 0, LCLK, 0,
};

static struct pointer_totals reported;
static struct pointer_totals relayed;
static uint8_t relayed_buttons[BUTTON_CHANGES_MAX];
static size_t relayed_button_count;
static uint8_t last_relayed_buttons;
static atomic_t relay_polls;
static atomic_t polls_at_press;
static atomic_t tap_inside_poll;

static void watch_mouse_key_tap(struct input_event *event, void *user_data) {
    ARG_UNUSED(user_data);
    if (event->type != INPUT_EV_KEY) {
        return;
    }
    atomic_val_t polls = atomic_get(&relay_polls);
    if (event->value != 0) {
        atomic_set(&polls_at_press, polls);
    } else if (atomic_get(&polls_at_press) == polls) {
        atomic_set(&tap_inside_poll, 1);
    }
}
INPUT_CALLBACK_DEFINE_NAMED(DEVICE_DT_GET(DT_NODELABEL(mkp)), watch_mouse_key_tap, NULL,
                            mouse_key_press);

static void add_motion(struct pointer_totals *totals, uint16_t code, int32_t value) {
    switch (code) {
    case INPUT_REL_X:
        totals->d_x += value;
        break;
    case INPUT_REL_Y:
        totals->d_y += value;
        break;
    case INPUT_REL_WHEEL:
        totals->d_scroll_y += value;
        break;
    case INPUT_REL_HWHEEL:
        totals->d_scroll_x += value;
        break;
    default:
        /* mouse move and scroll report no other relative code */
        break;
    }
}

static void count_reported_motion(struct input_event *event, void *user_data) {
    ARG_UNUSED(user_data);
    if (event->type == INPUT_EV_REL) {
        add_motion(&reported, event->code, event->value);
    }
}
INPUT_CALLBACK_DEFINE_NAMED(DEVICE_DT_GET(DT_NODELABEL(mmv)), count_reported_motion, NULL,
                            mouse_move);
INPUT_CALLBACK_DEFINE_NAMED(DEVICE_DT_GET(DT_NODELABEL(msc)), count_reported_motion, NULL,
                            mouse_scroll);

static size_t report_length(uint8_t report_id) {
    if (report_id == ZMK_HID_REPORT_ID_KEYBOARD) {
        return sizeof(struct zmk_hid_keyboard_report);
    }
    if (report_id == ZMK_HID_REPORT_ID_CONSUMER) {
        return sizeof(struct zmk_hid_consumer_report);
    }
    if (report_id == ZMK_HID_REPORT_ID_MOUSE) {
        return sizeof(struct zmk_hid_mouse_report);
    }
    return 0;
}

static void note_buttons(uint8_t buttons) {
    if (buttons == last_relayed_buttons) {
        return;
    }
    last_relayed_buttons = buttons;
    if (relayed_button_count == ARRAY_SIZE(relayed_buttons)) {
        printk("FAIL: more than %u relayed button changes\n", (unsigned int)BUTTON_CHANGES_MAX);
        exit(1);
    }
    relayed_buttons[relayed_button_count] = buttons;
    relayed_button_count++;
}

static bool report_moves(const struct zmk_hid_mouse_report *report) {
    const struct zmk_hid_mouse_report_body *body = &report->body;
    return body->d_x != 0 || body->d_y != 0 || body->d_scroll_y != 0 || body->d_scroll_x != 0;
}

static void add_pointer_report(const struct zmk_hid_mouse_report *report) {
    relayed.d_x += report->body.d_x;
    relayed.d_y += report->body.d_y;
    relayed.d_scroll_y += report->body.d_scroll_y;
    relayed.d_scroll_x += report->body.d_scroll_x;
    note_buttons(report->body.buttons);
}

int esb_write_payload(const struct esb_payload *payload) {
    size_t offset = 0;
    size_t moving_reports = 0;
    while (offset < payload->length) {
        uint8_t report_id = payload->data[offset + REPORT_ID_OFFSET];
        size_t length = report_length(report_id);
        if (length == 0 || offset + length > payload->length) {
            printk("FAIL: relay ACK of %u bytes is not whole HID reports\n",
                   (unsigned int)payload->length);
            exit(1);
        }
        if (report_id == ZMK_HID_REPORT_ID_MOUSE) {
            struct zmk_hid_mouse_report report = {0};
            memcpy(&report, &payload->data[offset], sizeof(report));
            add_pointer_report(&report);
            moving_reports += report_moves(&report) ? 1 : 0;
        }
        offset += length;
    }
    if (moving_reports > 1) {
        printk("FAIL: one relay ACK carries %u pointer reports with motion\n",
               (unsigned int)moving_reports);
        exit(1);
    }
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

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    esb_link_role_rx_done((uint8_t)BIT(RELAY_PIPE));
    atomic_inc(&relay_polls);
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
}

static void print_buttons(const char *label, const uint8_t *buttons, size_t count) {
    printk("%s", label);
    for (size_t index = 0; index < count; index++) {
        printk(" 0x%02x", buttons[index]);
    }
    printk("\n");
}

static void check_tap_inside_poll(void) {
    if (atomic_get(&tap_inside_poll) != 0) {
        return;
    }
    printk("FAIL: no mouse key went down and up between two relay polls\n");
    exit(1);
}

static void check_buttons(void) {
    if (relayed_button_count == ARRAY_SIZE(expected_buttons) &&
        memcmp(relayed_buttons, expected_buttons, sizeof(expected_buttons)) == 0) {
        return;
    }
    printk("FAIL: relay ACKs lost or reordered a mouse key button change\n");
    print_buttons("relayed:", relayed_buttons, relayed_button_count);
    print_buttons("expected:", expected_buttons, ARRAY_SIZE(expected_buttons));
    exit(1);
}

static void check_mouse_keys_moved(void) {
    if (reported.d_x > 0 && reported.d_y == 0 && reported.d_scroll_y > 0 &&
        reported.d_scroll_x == 0) {
        return;
    }
    printk("FAIL: mouse keys reported x %d y %d wheel %d hwheel %d, "
           "expected rightward motion and upward scroll\n",
           (int)reported.d_x, (int)reported.d_y, (int)reported.d_scroll_y,
           (int)reported.d_scroll_x);
    exit(1);
}

static void check_motion_relayed(void) {
    if (relayed.d_x == reported.d_x && relayed.d_y == reported.d_y &&
        relayed.d_scroll_y == reported.d_scroll_y && relayed.d_scroll_x == reported.d_scroll_x) {
        return;
    }
    printk("FAIL: relayed x %d y %d wheel %d hwheel %d, "
           "mouse keys reported x %d y %d wheel %d hwheel %d\n",
           (int)relayed.d_x, (int)relayed.d_y, (int)relayed.d_scroll_y, (int)relayed.d_scroll_x,
           (int)reported.d_x, (int)reported.d_y, (int)reported.d_scroll_y,
           (int)reported.d_scroll_x);
    exit(1);
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    check_tap_inside_poll();
    check_mouse_keys_moved();
    check_buttons();
    check_motion_relayed();
    printk("PASS: relay ACKs carried %u mouse key button changes in order, "
           "every move and scroll count\n",
           (unsigned int)relayed_button_count);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_START_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
