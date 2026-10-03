// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on the central.
 * Exits 0 once the relay delivers the expected report changes in order.
 * Exits 1 on the first wrong change or at deadline.
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

#include <dt-bindings/zmk/keys.h>
#include <zmk/hid.h>

#include <esb.h>

#include "esb_link_internal.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define VERDICT_DEADLINE_MS 1000
#define REPORT_ID_OFFSET offsetof(struct zmk_hid_keyboard_report, report_id)
#define RELEASED 0

const uint8_t esb_link_pipe_count = DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals));

struct expected_change {
    uint8_t report_id;
    uint32_t usage;
};

static const struct expected_change expected_changes[] = {
    {.report_id = ZMK_HID_REPORT_ID_KEYBOARD, .usage = A},
    {.report_id = ZMK_HID_REPORT_ID_KEYBOARD, .usage = RELEASED},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER, .usage = C_MUTE},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER, .usage = RELEASED},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER, .usage = C_VOL_UP},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER, .usage = RELEASED},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER, .usage = C_VOL_UP},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER, .usage = RELEASED},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER, .usage = C_VOL_DN},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER, .usage = RELEASED},
};
static size_t next_change;

union relay_report {
    struct zmk_hid_keyboard_report keyboard;
    struct zmk_hid_consumer_report consumer;
};

static struct zmk_hid_keyboard_report last_keyboard = {.report_id = ZMK_HID_REPORT_ID_KEYBOARD};
static struct zmk_hid_consumer_report last_consumer = {.report_id = ZMK_HID_REPORT_ID_CONSUMER};

static size_t build_expected(const struct expected_change *change, union relay_report *report) {
    *report = (union relay_report){0};
    if (change->report_id == ZMK_HID_REPORT_ID_KEYBOARD) {
        report->keyboard.report_id = ZMK_HID_REPORT_ID_KEYBOARD;
        if (change->usage != RELEASED) {
            report->keyboard.body.keys[0] = ZMK_HID_USAGE_ID(change->usage);
        }
        return sizeof(report->keyboard);
    }
    report->consumer.report_id = ZMK_HID_REPORT_ID_CONSUMER;
    if (change->usage != RELEASED) {
        report->consumer.body.keys[0] = ZMK_HID_USAGE_ID(change->usage);
    }
    return sizeof(report->consumer);
}

static void print_bytes(const uint8_t *data, size_t length) {
    for (size_t index = 0; index < length; index++) {
        printk(" %02x", data[index]);
    }
    printk("\n");
}

static void check_change(const uint8_t *data, size_t length) {
    union relay_report expected;
    size_t expected_length = build_expected(&expected_changes[next_change], &expected);
    if (length != expected_length || memcmp(data, &expected, length) != 0) {
        printk("FAIL: step %u of %u, expected", (unsigned int)(next_change + 1),
               (unsigned int)ARRAY_SIZE(expected_changes));
        print_bytes((const uint8_t *)&expected, expected_length);
        printk("FAIL: got");
        print_bytes(data, length);
        exit(1);
    }
    next_change++;
    if (next_change == ARRAY_SIZE(expected_changes)) {
        printk("PASS: relay delivered all %u report changes in order\n",
               (unsigned int)ARRAY_SIZE(expected_changes));
        exit(0);
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    void *last;
    size_t size;
    if (payload->data[REPORT_ID_OFFSET] == ZMK_HID_REPORT_ID_KEYBOARD) {
        last = &last_keyboard;
        size = sizeof(last_keyboard);
    } else if (payload->data[REPORT_ID_OFFSET] == ZMK_HID_REPORT_ID_CONSUMER) {
        last = &last_consumer;
        size = sizeof(last_consumer);
    } else {
        return 0;
    }
    if (payload->length != size) {
        return -EMSGSIZE;
    }
    if (memcmp(last, payload->data, size) == 0) {
        return 0;
    }
    memcpy(last, payload->data, size);
    check_change(payload->data, size);
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
    printk("FAIL: relay stopped at step %u of %u\n", (unsigned int)(next_change + 1),
           (unsigned int)ARRAY_SIZE(expected_changes));
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
