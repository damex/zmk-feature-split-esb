// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a central relaying HID to a dongle.
 * A report written during poll N rides the ACK of poll N + 1.
 * Exits 0 once every burst change is written at the first poll after its key event.
 * Exits 1 on a late change, a re-send ahead of a waiting change or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

#include <esb.h>

#include "esb_link_internal.h"

#define VERDICT_DEADLINE_MS 1000
#define CHANGES_EXPECTED 4

const uint8_t esb_link_pipe_count = DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals));

static uint32_t polls_done;
static uint32_t change_event_poll[CHANGES_EXPECTED];
static size_t changes_seen;
static size_t changes_written;
static struct zmk_hid_keyboard_report last_keyboard = {.report_id = ZMK_HID_REPORT_ID_KEYBOARD};
static struct zmk_hid_consumer_report last_consumer = {.report_id = ZMK_HID_REPORT_ID_CONSUMER};

static int keycode_listener(const zmk_event_t *event) {
    if (as_zmk_keycode_state_changed(event) == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (changes_seen < CHANGES_EXPECTED) {
        change_event_poll[changes_seen] = polls_done;
        changes_seen++;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_relay_burst_test, keycode_listener);
ZMK_SUBSCRIPTION(hid_relay_burst_test, zmk_keycode_state_changed);

static void *last_report_for(uint8_t report_id, size_t *length) {
    if (report_id == ZMK_HID_REPORT_ID_KEYBOARD) {
        *length = sizeof(last_keyboard);
        return &last_keyboard;
    }
    if (report_id == ZMK_HID_REPORT_ID_CONSUMER) {
        *length = sizeof(last_consumer);
        return &last_consumer;
    }
    *length = 0;
    return NULL;
}

static void check_report(const uint8_t *report, void *last, size_t length) {
    if (memcmp(last, report, length) == 0) {
        if (changes_written < changes_seen) {
            printk("FAIL: re-send written at poll %u while change %u of %u waits\n",
                   (unsigned int)polls_done, (unsigned int)(changes_written + 1),
                   (unsigned int)CHANGES_EXPECTED);
            exit(1);
        }
        return;
    }
    memcpy(last, report, length);
    if (changes_written == changes_seen) {
        printk("FAIL: report changed at poll %u without a key change\n", (unsigned int)polls_done);
        exit(1);
    }
    uint32_t due_poll = change_event_poll[changes_written] + 1;
    if (polls_done != due_poll) {
        printk("FAIL: change %u of %u written at poll %u, due at poll %u\n",
               (unsigned int)(changes_written + 1), (unsigned int)CHANGES_EXPECTED,
               (unsigned int)polls_done, (unsigned int)due_poll);
        exit(1);
    }
    changes_written++;
    if (changes_written == CHANGES_EXPECTED) {
        printk("PASS: all %u burst changes written at the first poll after their key event\n",
               (unsigned int)CHANGES_EXPECTED);
        exit(0);
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    size_t offset = 0;
    while (offset < payload->length) {
        size_t length = 0;
        void *last = last_report_for(payload->data[offset], &length);
        if (last == NULL || offset + length > payload->length) {
            printk("FAIL: reply at poll %u does not split into whole reports\n",
                   (unsigned int)polls_done);
            exit(1);
        }
        check_report(&payload->data[offset], last, length);
        offset += length;
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
    polls_done++;
    esb_link_role_rx_done((uint8_t)BIT_MASK(esb_link_pipe_count));
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: %u of %u burst changes written by deadline\n", (unsigned int)changes_written,
           (unsigned int)CHANGES_EXPECTED);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
