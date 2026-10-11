// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a HID relay dongle.
 * Relay ACKs arrive every RELAY_POLL_MS while the pointer moves and keys change.
 * Fake esb_hid_relay_usb.c endpoint: one report per HOST_FRAME_MS,
 * a sender waits up to ENDPOINT_WAIT_MS for it.
 * Exits 0 once every key change reaches the sink in order within one relay poll
 * and the sink's motion adds up to the totals sent.
 * Exits 1 on a skipped, reordered or late key change, missing motion or at deadline.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

#include <esb.h>

#include "mock.h"
#include "mock_esb.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define RELAY_POLL_MS 2
#define REFRESH_POLLS 2
#define KEY_CHANGE_POLLS 4
#define LOAD_POLLS 250
#define HOST_FRAME_MS 1
#define ENDPOINT_WAIT_MS 10
#define LOAD_START_MS 50
#define SETTLE_MS 200
#define VERDICT_MS (LOAD_START_MS + LOAD_POLLS * RELAY_POLL_MS + SETTLE_MS)
#define KEY_CHANGES (LOAD_POLLS / KEY_CHANGE_POLLS)
#define KEY_LATENCY_BOUND_MS RELAY_POLL_MS
#define FIRST_TOTAL_X 1

BUILD_ASSERT(HID_USAGE_KEY_KEYBOARD_A + KEY_CHANGES <= UINT8_MAX,
             "every key change needs its own usage");
BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report) + sizeof(struct zmk_hid_consumer_report) +
                     sizeof(struct zmk_hid_mouse_report) <=
                 CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "refresh and pointer reports share one ACK");

static uint32_t polls_sent;
static int16_t total_x;
static size_t key_changes_sent;
static uint32_t key_change_sent_ms[KEY_CHANGES];

static K_SEM_DEFINE(endpoint_free, 1, 1);
static atomic_t report_in_flight;
static size_t endpoint_timeouts;
static size_t next_key_change;
static size_t key_changes_skipped;
static uint32_t max_key_latency_ms;
static int32_t sink_x;

static void append_report(struct esb_payload *payload, const void *report, size_t length) {
    memcpy(&payload->data[payload->length], report, length);
    payload->length = (uint8_t)(payload->length + length);
}

static struct zmk_hid_keyboard_report keyboard_state(void) {
    struct zmk_hid_keyboard_report report = {.report_id = ZMK_HID_REPORT_ID_KEYBOARD};
    if (key_changes_sent > 0) {
        report.body.keys[0] = (uint8_t)(HID_USAGE_KEY_KEYBOARD_A + key_changes_sent - 1);
    }
    return report;
}

static void append_idle_refresh(struct esb_payload *payload) {
    struct zmk_hid_keyboard_report keyboard = keyboard_state();
    struct zmk_hid_consumer_report consumer = {.report_id = ZMK_HID_REPORT_ID_CONSUMER};
    append_report(payload, &keyboard, sizeof(keyboard));
    append_report(payload, &consumer, sizeof(consumer));
}

static void append_key_change(struct esb_payload *payload) {
    key_change_sent_ms[key_changes_sent] = k_uptime_get_32();
    key_changes_sent++;
    struct zmk_hid_keyboard_report keyboard = keyboard_state();
    append_report(payload, &keyboard, sizeof(keyboard));
}

static void relay_poll_fn(struct k_timer *timer) {
    if (polls_sent == LOAD_POLLS) {
        k_timer_stop(timer);
        return;
    }
    polls_sent++;
    total_x++;
    struct esb_payload payload = {.pipe = SELF_PIPE};
    if (polls_sent % KEY_CHANGE_POLLS == 0) {
        append_key_change(&payload);
    } else if (polls_sent % REFRESH_POLLS == 0) {
        append_idle_refresh(&payload);
    }
    struct zmk_hid_mouse_report mouse = {
        .report_id = ZMK_HID_REPORT_ID_MOUSE,
        .body = {.d_x = total_x},
    };
    append_report(&payload, &mouse, sizeof(mouse));
    mock_esb_rx_push(&payload);
    mock_esb_rx_raise();
}
static K_TIMER_DEFINE(relay_poll_timer, relay_poll_fn, NULL);

static void host_frame_fn(struct k_timer *timer) {
    ARG_UNUSED(timer);
    if (atomic_cas(&report_in_flight, 1, 0)) {
        k_sem_give(&endpoint_free);
    }
}
static K_TIMER_DEFINE(host_frame_timer, host_frame_fn, NULL);

static void sink_keyboard(const uint8_t *bytes) {
    struct zmk_hid_keyboard_report report = {0};
    memcpy(&report, bytes, sizeof(report));
    size_t index = (size_t)report.body.keys[0] - HID_USAGE_KEY_KEYBOARD_A;
    if (report.body.keys[0] < HID_USAGE_KEY_KEYBOARD_A || index < next_key_change ||
        index >= key_changes_sent) {
        printk("FAIL: key change %u reached the sink after change %u\n", (unsigned int)index,
               (unsigned int)next_key_change);
        exit(1);
    }
    key_changes_skipped += index - next_key_change;
    max_key_latency_ms = MAX(max_key_latency_ms, k_uptime_get_32() - key_change_sent_ms[index]);
    next_key_change = index + 1;
}

static void sink_mouse(const uint8_t *bytes) {
    struct zmk_hid_mouse_report report = {0};
    memcpy(&report, bytes, sizeof(report));
    sink_x += report.body.d_x;
}

static int sink(const uint8_t *bytes, size_t length) {
    if (k_sem_take(&endpoint_free, K_MSEC(ENDPOINT_WAIT_MS)) != 0) {
        endpoint_timeouts++;
        return -EBUSY;
    }
    atomic_set(&report_in_flight, 1);
    if (bytes[0] == ZMK_HID_REPORT_ID_KEYBOARD && length == sizeof(struct zmk_hid_keyboard_report)) {
        sink_keyboard(bytes);
    } else if (bytes[0] == ZMK_HID_REPORT_ID_MOUSE &&
               length == sizeof(struct zmk_hid_mouse_report)) {
        sink_mouse(bytes);
    }
    return 0;
}

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    int32_t expected_x = total_x - FIRST_TOTAL_X;
    bool passed = key_changes_skipped == 0 && next_key_change == key_changes_sent &&
                  max_key_latency_ms <= KEY_LATENCY_BOUND_MS && sink_x == expected_x;
    printk("%s: %u of %u key changes skipped, max key latency %u ms, motion %d of %d, "
           "%u endpoint timeouts\n",
           passed ? "PASS" : "FAIL", (unsigned int)key_changes_skipped,
           (unsigned int)key_changes_sent, (unsigned int)max_key_latency_ms, (int)sink_x,
           (int)expected_x, (unsigned int)endpoint_timeouts);
    exit(passed ? 0 : 1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    mock_check(zmk_split_esb_hid_relay_register(sink) == 0, "sink registers");
    k_timer_start(&host_frame_timer, K_MSEC(HOST_FRAME_MS), K_MSEC(HOST_FRAME_MS));
    k_timer_start(&relay_poll_timer, K_MSEC(LOAD_START_MS), K_MSEC(RELAY_POLL_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
