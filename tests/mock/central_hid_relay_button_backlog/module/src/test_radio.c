// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central with a relay dongle, a key and ZMK's mouse key press.
 * Burst inside one relay poll: LCLK down, A down, RCLK down, A up, LCLK up, RCLK up.
 * First ACK fills before the A release, with room left for one more pointer report.
 * Exits 0 once the dongle sees every button change in order.
 * Exits 1 on an unknown report, a burst spread across relay polls,
 * or current buttons overtaking a queued button change.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
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
#define RELAY_POLL_MS 40
#define VERDICT_MS 400
#define BUTTON_CHANGES_MAX 16
#define POINTER_LENGTH sizeof(struct zmk_hid_mouse_report)
#define KEYBOARD_LENGTH sizeof(struct zmk_hid_keyboard_report)
#define BURST_FIRST_ACK_LENGTH (POINTER_LENGTH + KEYBOARD_LENGTH + POINTER_LENGTH)

BUILD_ASSERT(BURST_FIRST_ACK_LENGTH + KEYBOARD_LENGTH > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "A release must not fit the burst's first ACK");
BUILD_ASSERT(BURST_FIRST_ACK_LENGTH + POINTER_LENGTH <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "burst's first ACK must have room for one more pointer report");

static const uint8_t expected_buttons[] = {
    LCLK, LCLK | RCLK, RCLK, 0,
};

static uint8_t relayed_buttons[BUTTON_CHANGES_MAX];
static size_t relayed_button_count;
static uint8_t dongle_buttons;
static atomic_t relay_polls;
static atomic_t mouse_key_events;
static atomic_t polls_at_first_mouse_key;
static atomic_t polls_at_last_mouse_key;

static void watch_mouse_keys(struct input_event *event, void *user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
    atomic_val_t polls = atomic_get(&relay_polls);
    if (atomic_inc(&mouse_key_events) == 0) {
        atomic_set(&polls_at_first_mouse_key, polls);
    }
    atomic_set(&polls_at_last_mouse_key, polls);
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(mkp)), watch_mouse_keys, NULL);

static size_t report_length(uint8_t report_id) {
    if (report_id == ZMK_HID_REPORT_ID_KEYBOARD) {
        return KEYBOARD_LENGTH;
    }
    if (report_id == ZMK_HID_REPORT_ID_CONSUMER) {
        return sizeof(struct zmk_hid_consumer_report);
    }
    if (report_id == ZMK_HID_REPORT_ID_MOUSE) {
        return POINTER_LENGTH;
    }
    return 0;
}

static void note_buttons(uint8_t buttons) {
    if (buttons == dongle_buttons) {
        return;
    }
    dongle_buttons = buttons;
    if (relayed_button_count == ARRAY_SIZE(relayed_buttons)) {
        printk("FAIL: more than %u relayed button changes\n", (unsigned int)BUTTON_CHANGES_MAX);
        exit(1);
    }
    relayed_buttons[relayed_button_count] = buttons;
    relayed_button_count++;
}

int esb_write_payload(const struct esb_payload *payload) {
    size_t offset = 0;
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
            note_buttons(report.body.buttons);
        }
        offset += length;
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
    k_work_reschedule(&relay_poll_work, K_MSEC(RELAY_POLL_MS));
}

static void print_buttons(const char *label, const uint8_t *buttons, size_t count) {
    printk("%s", label);
    for (size_t index = 0; index < count; index++) {
        printk(" 0x%02x", buttons[index]);
    }
    printk("\n");
}

static void check_burst_inside_poll(void) {
    if (atomic_get(&polls_at_first_mouse_key) == atomic_get(&polls_at_last_mouse_key)) {
        return;
    }
    printk("FAIL: mouse key burst spread across relay polls\n");
    exit(1);
}

static void check_buttons(void) {
    if (relayed_button_count == ARRAY_SIZE(expected_buttons) &&
        memcmp(relayed_buttons, expected_buttons, sizeof(expected_buttons)) == 0) {
        return;
    }
    printk("FAIL: dongle button changes differ from the queued ones\n");
    print_buttons("relayed:", relayed_buttons, relayed_button_count);
    print_buttons("expected:", expected_buttons, ARRAY_SIZE(expected_buttons));
    exit(1);
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    check_burst_inside_poll();
    check_buttons();
    printk("PASS: dongle saw the %u button changes of a burst split across two ACKs in order\n",
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
