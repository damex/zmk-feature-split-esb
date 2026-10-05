// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a HID relay dongle, radio payload above the link's.
 * Exits 0 once a packet past the link's payload size is dropped whole and the next one delivers.
 * Exits 1 when any report of the oversized packet reaches the sink, or at deadline.
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
#define REPORT_LENGTH sizeof(struct zmk_hid_keyboard_report)
#define OVERSIZE_REPORTS (CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD / REPORT_LENGTH + 1)
#define FOLLOW_UP_INDEX OVERSIZE_REPORTS
#define DELIVER_DELAY_MS 50
#define SETTLE_MS 100
#define VERDICT_DEADLINE_MS 1000

BUILD_ASSERT(OVERSIZE_REPORTS * REPORT_LENGTH > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "oversized packet must exceed the link's payload");
BUILD_ASSERT(OVERSIZE_REPORTS * REPORT_LENGTH <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "oversized packet must still fit the radio's payload");

static size_t delivered;

static struct zmk_hid_keyboard_report keyboard_report(size_t index) {
    struct zmk_hid_keyboard_report report = {.report_id = ZMK_HID_REPORT_ID_KEYBOARD};
    report.body.keys[0] = (uint8_t)(HID_USAGE_KEY_KEYBOARD_A + index);
    return report;
}

static int sink(const uint8_t *bytes, size_t length) {
    struct zmk_hid_keyboard_report follow_up = keyboard_report(FOLLOW_UP_INDEX);
    if (delivered > 0 || length != sizeof(follow_up) ||
        memcmp(bytes, &follow_up, length) != 0) {
        printk("FAIL: report from the oversized packet reached the sink:");
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
    mock_check(delivered == 1, "next packet within the link's payload still delivers");
    printk("PASS: %u-byte packet dropped whole, next packet delivered\n",
           (unsigned int)(OVERSIZE_REPORTS * REPORT_LENGTH));
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static void follow_up_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(delivered == 0, "oversized packet dropped whole");
    struct zmk_hid_keyboard_report report = keyboard_report(FOLLOW_UP_INDEX);
    struct esb_payload payload = {.pipe = SELF_PIPE, .length = sizeof(report)};
    memcpy(payload.data, &report, sizeof(report));
    mock_esb_rx_push(&payload);
    mock_esb_rx_raise();
    k_work_reschedule(&verdict_work, K_MSEC(SETTLE_MS));
}
static K_WORK_DELAYABLE_DEFINE(follow_up_work, follow_up_fn);

static void oversize_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct esb_payload payload = {.pipe = SELF_PIPE, .length = OVERSIZE_REPORTS * REPORT_LENGTH};
    for (size_t index = 0; index < OVERSIZE_REPORTS; index++) {
        struct zmk_hid_keyboard_report report = keyboard_report(index);
        memcpy(&payload.data[index * REPORT_LENGTH], &report, sizeof(report));
    }
    mock_esb_rx_push(&payload);
    mock_esb_rx_raise();
    k_work_reschedule(&follow_up_work, K_MSEC(SETTLE_MS));
}
static K_WORK_DELAYABLE_DEFINE(oversize_work, oversize_fn);

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: %u reports delivered by deadline\n", (unsigned int)delivered);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    mock_check(zmk_split_esb_hid_relay_register(sink) == 0, "sink registers");
    k_work_reschedule(&oversize_work, K_MSEC(DELIVER_DELAY_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
