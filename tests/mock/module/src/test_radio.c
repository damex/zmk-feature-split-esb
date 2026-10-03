// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on the central.
 * Exits 0 once the expected keyboard and consumer taps reach the relay, 1 at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/hid.h>

#include <esb.h>

#include "esb_link_internal.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define VERDICT_DEADLINE_MS 1000
#define REPORT_ID_OFFSET offsetof(struct zmk_hid_keyboard_report, report_id)
#define REPORT_BODY_OFFSET offsetof(struct zmk_hid_keyboard_report, body)

const uint8_t esb_link_pipe_count = DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals));

struct tracked_report {
    uint8_t report_id;
    uint32_t expected_taps;
};

static const struct tracked_report tracked_reports[] = {
    {.report_id = ZMK_HID_REPORT_ID_KEYBOARD,
     .expected_taps = CONFIG_ZMK_SPLIT_ESB_TEST_KEYBOARD_TAPS},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER,
     .expected_taps = CONFIG_ZMK_SPLIT_ESB_TEST_CONSUMER_TAPS},
};
static bool report_pressed[ARRAY_SIZE(tracked_reports)];
static uint32_t report_taps[ARRAY_SIZE(tracked_reports)];
static const uint8_t zero_bytes[CONFIG_ESB_MAX_PAYLOAD_LENGTH];

static bool report_is_empty(const struct esb_payload *payload) {
    size_t body_length = payload->length - REPORT_BODY_OFFSET;
    return memcmp(&payload->data[REPORT_BODY_OFFSET], zero_bytes, body_length) == 0;
}

static bool all_taps_delivered(void) {
    for (size_t index = 0; index < ARRAY_SIZE(tracked_reports); index++) {
        if (report_taps[index] < tracked_reports[index].expected_taps) {
            return false;
        }
    }
    return true;
}

int esb_write_payload(const struct esb_payload *payload) {
    if (payload->length <= REPORT_BODY_OFFSET || payload->length > CONFIG_ESB_MAX_PAYLOAD_LENGTH) {
        return -EMSGSIZE;
    }
    for (size_t index = 0; index < ARRAY_SIZE(tracked_reports); index++) {
        if (payload->data[REPORT_ID_OFFSET] != tracked_reports[index].report_id) {
            continue;
        }
        if (!report_is_empty(payload)) {
            report_pressed[index] = true;
        } else if (report_pressed[index]) {
            report_pressed[index] = false;
            report_taps[index]++;
        }
    }
    if (all_taps_delivered()) {
        printk("PASS: relay delivered every keyboard and consumer tap\n");
        exit(0);
    }
    return 0;
}

int esb_start_rx(void) {
    return 0;
}

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    esb_link_role_rx_done((uint8_t)BIT_MASK(esb_link_pipe_count));
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    for (size_t index = 0; index < ARRAY_SIZE(tracked_reports); index++) {
        if (report_taps[index] < tracked_reports[index].expected_taps) {
            printk("FAIL: relay delivered %u of %u taps on report 0x%02x\n",
                   report_taps[index], tracked_reports[index].expected_taps,
                   tracked_reports[index].report_id);
        }
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
