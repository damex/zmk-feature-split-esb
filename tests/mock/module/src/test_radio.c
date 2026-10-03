// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on the central.
 * Exits 0 once a pressed report reaches the relay followed by an empty one, 1 at deadline.
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
#define REPORT_BODY_OFFSET offsetof(struct zmk_hid_keyboard_report, body)

const uint8_t esb_link_pipe_count = DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals));

static const struct zmk_hid_keyboard_report_body empty_body;
static bool press_delivered;

static bool report_is_empty(const struct esb_payload *payload) {
    return memcmp(&payload->data[REPORT_BODY_OFFSET], &empty_body, sizeof(empty_body)) == 0;
}

int esb_write_payload(const struct esb_payload *payload) {
    if (payload->length != sizeof(struct zmk_hid_keyboard_report)) {
        return -EMSGSIZE;
    }
    if (!report_is_empty(payload)) {
        press_delivered = true;
        return 0;
    }
    if (press_delivered) {
        printk("PASS: relay delivered tap press and release\n");
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
    printk("FAIL: relay never delivered tap press\n");
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
