// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a HID relay dongle.
 * Exits 0 once one RX event past the ring depth delivers the first ring's worth in order,
 * drops the rest, and a later event still gets through.
 * Exits 1 on a wrong, extra or missing report, or at deadline.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

#include <esb.h>

#include "mock.h"
#include "mock_esb.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define RING_DEPTH CONFIG_ZMK_SPLIT_ESB_RX_QUEUE_SIZE
#define BURST_REPORTS (2 * RING_DEPTH)
#define RECOVERY_INDEX BURST_REPORTS
#define DELIVER_DELAY_MS 50
#define SETTLE_MS 100
#define VERDICT_DEADLINE_MS 1000

static size_t delivered;

static struct esb_payload report_payload(size_t index) {
    struct zmk_hid_keyboard_report report = {.report_id = ZMK_HID_REPORT_ID_KEYBOARD};
    report.body.keys[0] = (uint8_t)(HID_USAGE_KEY_KEYBOARD_A + index);
    struct esb_payload payload = {.pipe = SELF_PIPE, .length = sizeof(report)};
    memcpy(payload.data, &report, sizeof(report));
    return payload;
}

static size_t expected_index(size_t delivery) {
    if (delivery < RING_DEPTH) {
        return delivery;
    }
    return RECOVERY_INDEX;
}

static int sink(const uint8_t *bytes, size_t length) {
    if (delivered > RING_DEPTH) {
        printk("FAIL: extra report reached the sink:");
        mock_print_bytes(bytes, length);
        exit(1);
    }
    struct esb_payload expected = report_payload(expected_index(delivered));
    if (length != expected.length || memcmp(bytes, expected.data, length) != 0) {
        printk("FAIL: delivery %u, expected", (unsigned int)(delivered + 1));
        mock_print_bytes(expected.data, expected.length);
        printk("FAIL: got");
        mock_print_bytes(bytes, length);
        exit(1);
    }
    delivered++;
    return 0;
}

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(delivered == RING_DEPTH + 1, "ring takes a packet again after the overflow");
    printk("PASS: %u of %u burst reports delivered in order, the rest dropped, ring recovered\n",
           (unsigned int)RING_DEPTH, (unsigned int)BURST_REPORTS);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static void recovery_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(delivered == RING_DEPTH, "burst delivers one ring of reports");
    struct esb_payload payload = report_payload(RECOVERY_INDEX);
    mock_esb_rx_push(&payload);
    mock_esb_rx_raise();
    k_work_reschedule(&verdict_work, K_MSEC(SETTLE_MS));
}
static K_WORK_DELAYABLE_DEFINE(recovery_work, recovery_fn);

static void burst_fn(struct k_work *work) {
    ARG_UNUSED(work);
    for (size_t index = 0; index < BURST_REPORTS; index++) {
        struct esb_payload payload = report_payload(index);
        mock_esb_rx_push(&payload);
    }
    mock_esb_rx_raise();
    k_work_reschedule(&recovery_work, K_MSEC(SETTLE_MS));
}
static K_WORK_DELAYABLE_DEFINE(burst_work, burst_fn);

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: %u reports delivered by deadline\n", (unsigned int)delivered);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    mock_check(zmk_split_esb_hid_relay_register(sink) == 0, "sink registers");
    k_work_reschedule(&burst_work, K_MSEC(DELIVER_DELAY_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
