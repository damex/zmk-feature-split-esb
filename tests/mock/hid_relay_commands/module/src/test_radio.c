// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a central relaying HID to a dongle.
 * Exits 0 once a behavior sent to the relay source never reaches its replies.
 * Exits 1 on a reply that is not whole HID reports.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/split/central.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define INVOKE_DELAY_MS 50
#define POLLS_CHECKED 10
#define REPORT_ID_OFFSET 0

static uint32_t polls_done;

static size_t report_length(uint8_t report_id) {
    if (report_id == ZMK_HID_REPORT_ID_KEYBOARD) {
        return sizeof(struct zmk_hid_keyboard_report);
    }
    if (report_id == ZMK_HID_REPORT_ID_CONSUMER) {
        return sizeof(struct zmk_hid_consumer_report);
    }
    return 0;
}

int esb_write_payload(const struct esb_payload *payload) {
    size_t offset = 0;
    while (offset < payload->length) {
        size_t length = report_length(payload->data[offset + REPORT_ID_OFFSET]);
        if (length == 0 || offset + length > payload->length) {
            printk("FAIL: relay reply at poll %u carries %u bytes that are not whole HID reports\n",
                   (unsigned int)polls_done, (unsigned int)payload->length);
            exit(1);
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

static void invoke_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct zmk_behavior_binding binding = {.behavior_dev = "test_behavior"};
    struct zmk_behavior_binding_event event = {.timestamp = k_uptime_get()};
    int error = zmk_split_central_invoke_behavior(RELAY_PIPE, &binding, event, true);
    if (error != 0) {
        printk("FAIL: behavior invoke on relay source returned %d\n", error);
        exit(1);
    }
}
static K_WORK_DELAYABLE_DEFINE(invoke_work, invoke_fn);

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    polls_done++;
    esb_link_role_rx_done((uint8_t)BIT(RELAY_PIPE));
    if (polls_done == POLLS_CHECKED) {
        printk("PASS: behavior sent to the relay source never reached its replies\n");
        exit(0);
    }
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
}

static int test_radio_init(void) {
    k_work_reschedule(&invoke_work, K_MSEC(INVOKE_DELAY_MS));
    k_work_reschedule(&relay_poll_work,
                      K_MSEC(INVOKE_DELAY_MS + CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
