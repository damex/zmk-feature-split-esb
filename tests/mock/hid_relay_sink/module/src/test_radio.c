// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link on a HID relay dongle.
 * Exits 0 once the sink sees every report change once, in order, re-sends dropped,
 * a rejected report retried by its re-send.
 * Exits 1 on a duplicate, a wrong or a missing report.
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
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/keys.h>
#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

#include "esb_link.h"
#include "hop.h"
#include "mock.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define DELIVER_DELAY_MS 50
#define RELEASED 0
#define REJECTED_STEP 4

static const uint32_t expected_usages[] = {
    A,
    RELEASED,
    B,
    RELEASED,
    C,
    C,
};
static size_t next_expected;
static uint8_t last_sink_report[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
static size_t last_sink_length;

static esb_link_rx_callback_t rx_callback;

int esb_link_init(esb_link_rx_callback_t callback) {
    rx_callback = callback;
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    return 0;
}

int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    ARG_UNUSED(data);
    ARG_UNUSED(length);
    ARG_UNUSED(ack);
    return 0;
}

void hop_restore(void) {
}

uint8_t hop_link_cost_x10(void) {
    return 0;
}

static struct zmk_hid_keyboard_report keyboard_report(uint32_t usage) {
    struct zmk_hid_keyboard_report report = {.report_id = ZMK_HID_REPORT_ID_KEYBOARD};
    if (usage != RELEASED) {
        report.body.keys[0] = ZMK_HID_USAGE_ID(usage);
    }
    return report;
}

static int sink(const uint8_t *bytes, size_t length) {
    if (length == last_sink_length && memcmp(bytes, last_sink_report, length) == 0) {
        printk("FAIL: duplicate report reached the sink at step %u:",
               (unsigned int)(next_expected + 1));
        mock_print_bytes(bytes, length);
        exit(1);
    }
    if (next_expected == ARRAY_SIZE(expected_usages)) {
        printk("FAIL: extra report reached the sink:");
        mock_print_bytes(bytes, length);
        exit(1);
    }
    struct zmk_hid_keyboard_report expected = keyboard_report(expected_usages[next_expected]);
    if (length != sizeof(expected) || memcmp(bytes, &expected, length) != 0) {
        printk("FAIL: sink step %u of %u, expected", (unsigned int)(next_expected + 1),
               (unsigned int)ARRAY_SIZE(expected_usages));
        mock_print_bytes((const uint8_t *)&expected, sizeof(expected));
        printk("FAIL: got");
        mock_print_bytes(bytes, length);
        exit(1);
    }
    size_t step = next_expected;
    next_expected++;
    if (step == REJECTED_STEP) {
        return -EBUSY;
    }
    memcpy(last_sink_report, bytes, length);
    last_sink_length = length;
    return 0;
}

static void deliver(const void *reply, size_t length) {
    rx_callback(SELF_PIPE, reply, length);
}

static void deliver_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct zmk_hid_keyboard_report press_a = keyboard_report(A);
    struct zmk_hid_keyboard_report press_b = keyboard_report(B);
    struct zmk_hid_keyboard_report press_c = keyboard_report(C);
    struct zmk_hid_keyboard_report released = keyboard_report(RELEASED);
    struct zmk_hid_consumer_report consumer_idle = {.report_id = ZMK_HID_REPORT_ID_CONSUMER};
    uint8_t packed[sizeof(press_b) + sizeof(released)];
    memcpy(packed, &press_b, sizeof(press_b));
    memcpy(&packed[sizeof(press_b)], &released, sizeof(released));

    deliver(&press_a, sizeof(press_a));
    deliver(&press_a, sizeof(press_a));
    deliver(&consumer_idle, sizeof(consumer_idle));
    /* Release of A lost on air, the next re-send carries it. */
    deliver(&released, sizeof(released));
    deliver(packed, sizeof(packed));
    deliver(&released, sizeof(released));
    deliver(&press_c, sizeof(press_c));
    deliver(&press_c, sizeof(press_c));

    if (next_expected != ARRAY_SIZE(expected_usages)) {
        printk("FAIL: sink got %u of %u deliveries\n", (unsigned int)next_expected,
               (unsigned int)ARRAY_SIZE(expected_usages));
        exit(1);
    }
    printk("PASS: sink got all %u deliveries in order, re-sends dropped, rejected one retried\n",
           (unsigned int)ARRAY_SIZE(expected_usages));
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(deliver_work, deliver_fn);

static int test_radio_init(void) {
    int error = zmk_split_esb_hid_relay_register(sink);
    if (error != 0) {
        printk("FAIL: sink registration returned %d\n", error);
        exit(1);
    }
    k_work_reschedule(&deliver_work, K_MSEC(DELIVER_DELAY_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
