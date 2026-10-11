// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central relaying to the dongle's own delivery code,
 * behind a fake USB sink.
 * Motion every MOTION_PERIOD_MS while a key burst, all inside one RELAY_POLL_MS,
 * keeps relay reports queued across ACKs, a mouse button press right behind it.
 * One ACK with a mouse report in LOST_EVERY never reaches the dongle,
 * none while the key burst and the press are in flight.
 * Exits 0 once the sink got every count of motion, never a step back, the press after
 * every key change, and each ACK with key reports written during the motion carried
 * a mouse report too.
 * Exits 1 on motion short at the sink, a step back, a press ahead of a key change,
 * a key ACK without motion, or a report that is not whole.
 */
#include <errno.h>
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

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

#include <esb.h>

#include "esb_hid_relay_peripheral.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define POINTER_DEVICE DEVICE_DT_GET(DT_NODELABEL(pointer_input))
#define REPORT_ID_OFFSET 0
#define POLL_START_MS 20
#define RELAY_POLL_MS 20
#define MOTION_START_MS 250
#define MOTION_PERIOD_MS 2
#define MOTION_STEPS 100
#define LOST_EVERY 7
#define KEY_CHANGES 6
#define RELEASE_MS 400
#define PRIMARY_BUTTON BIT(0)
#define VERDICT_MS 700

struct ack_contents {
    bool keyboard;
    bool pointer;
};

static atomic_t motion_steps;
static int32_t sink_x;
static size_t sink_key_changes;
static bool sink_pressed;
static bool sink_press_seen;
static size_t key_changes;

static bool burst_in_flight(void) {
    return key_changes > 0 && !sink_press_seen;
}
static size_t pointer_acks;
static size_t lost_acks;
static size_t keyed_acks;

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

static struct ack_contents scan_ack(const struct esb_payload *payload) {
    struct ack_contents contents = {0};
    size_t offset = 0;
    while (offset < payload->length) {
        uint8_t report_id = payload->data[offset + REPORT_ID_OFFSET];
        size_t length = report_length(report_id);
        if (length == 0 || offset + length > payload->length) {
            printk("FAIL: relay ACK of %u bytes is not whole HID reports\n",
                   (unsigned int)payload->length);
            exit(1);
        }
        contents.keyboard |= report_id == ZMK_HID_REPORT_ID_KEYBOARD;
        contents.pointer |= report_id == ZMK_HID_REPORT_ID_MOUSE;
        offset += length;
    }
    return contents;
}

static bool motion_streaming(void) {
    atomic_val_t steps = atomic_get(&motion_steps);
    return steps > 0 && steps < MOTION_STEPS;
}

int esb_write_payload(const struct esb_payload *payload) {
    struct ack_contents contents = scan_ack(payload);
    if (contents.keyboard && motion_streaming()) {
        keyed_acks++;
        if (!contents.pointer) {
            printk("FAIL: relay ACK carried key reports and no motion while motion was pending\n");
            exit(1);
        }
    }
    if (contents.pointer && !burst_in_flight()) {
        pointer_acks++;
        if (pointer_acks % LOST_EVERY == 0) {
            lost_acks++;
            return 0;
        }
    }
    esb_hid_relay_deliver(payload->data, payload->length);
    return 0;
}

static void check_press_order(uint8_t buttons) {
    bool pressed = (buttons & PRIMARY_BUTTON) != 0;
    if (pressed && !sink_pressed && sink_key_changes != KEY_CHANGES) {
        printk("FAIL: sink got the press after %u of %u key changes queued ahead of it\n",
               (unsigned int)sink_key_changes, (unsigned int)KEY_CHANGES);
        exit(1);
    }
    sink_pressed = pressed;
    sink_press_seen |= pressed;
}

static int usb_sink(const uint8_t *bytes, size_t length) {
    if (bytes[REPORT_ID_OFFSET] == ZMK_HID_REPORT_ID_KEYBOARD) {
        sink_key_changes++;
        return 0;
    }
    struct zmk_hid_mouse_report report = {0};
    if (length != sizeof(report) || bytes[REPORT_ID_OFFSET] != ZMK_HID_REPORT_ID_MOUSE) {
        return 0;
    }
    memcpy(&report, bytes, sizeof(report));
    if (report.body.d_x < 0) {
        printk("FAIL: sink stepped back by %d\n", report.body.d_x);
        exit(1);
    }
    check_press_order(report.body.buttons);
    sink_x += report.body.d_x;
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
    k_work_reschedule(&relay_poll_work, K_MSEC(RELAY_POLL_MS));
}

static void motion_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(motion_work, motion_fn);

static void motion_fn(struct k_work *work) {
    ARG_UNUSED(work);
    input_report_rel(POINTER_DEVICE, INPUT_REL_X, 1, true, K_NO_WAIT);
    if (atomic_inc(&motion_steps) + 1 < MOTION_STEPS) {
        k_work_reschedule(&motion_work, K_MSEC(MOTION_PERIOD_MS));
    }
}

static void press_fn(struct k_work *work) {
    ARG_UNUSED(work);
    input_report_key(POINTER_DEVICE, INPUT_BTN_0, 1, true, K_NO_WAIT);
}
static K_WORK_DEFINE(press_work, press_fn);

static int keycode_listener(const zmk_event_t *event) {
    if (as_zmk_keycode_state_changed(event) == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    key_changes++;
    if (key_changes == KEY_CHANGES) {
        k_work_submit(&press_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(central_hid_relay_totals_test, keycode_listener);
ZMK_SUBSCRIPTION(central_hid_relay_totals_test, zmk_keycode_state_changed);

static void release_fn(struct k_work *work) {
    ARG_UNUSED(work);
    input_report_key(POINTER_DEVICE, INPUT_BTN_0, 0, true, K_NO_WAIT);
}
static K_WORK_DELAYABLE_DEFINE(release_work, release_fn);

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (sink_key_changes != KEY_CHANGES || !sink_press_seen) {
        printk("FAIL: sink got %u of %u key changes, press %s\n",
               (unsigned int)sink_key_changes, (unsigned int)KEY_CHANGES,
               sink_press_seen ? "seen" : "missing");
        exit(1);
    }
    if (keyed_acks == 0 || lost_acks == 0) {
        printk("FAIL: %u key ACKs during motion and %u lost motion ACKs, the test needs both\n",
               (unsigned int)keyed_acks, (unsigned int)lost_acks);
        exit(1);
    }
    if (sink_x != MOTION_STEPS) {
        printk("FAIL: sink got %d of %d counts, %u of %u motion ACKs lost\n", (int)sink_x,
               MOTION_STEPS, (unsigned int)lost_acks, (unsigned int)pointer_acks);
        exit(1);
    }
    printk("PASS: sink got all %d counts through %u lost ACKs, %u key ACKs carried motion\n",
           (int)sink_x, (unsigned int)lost_acks, (unsigned int)keyed_acks);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    int error = zmk_split_esb_hid_relay_register(usb_sink);
    if (error != 0) {
        printk("FAIL: sink registration returned %d\n", error);
        exit(1);
    }
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_START_MS));
    k_work_reschedule(&motion_work, K_MSEC(MOTION_START_MS));
    k_work_reschedule(&release_work, K_MSEC(RELEASE_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
