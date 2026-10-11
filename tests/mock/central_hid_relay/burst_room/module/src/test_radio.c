// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a central relaying HID to a dongle, relay pointer processor present and still.
 * Three NKRO changes land inside one RELAY_POLL_MS and fill one ACK whole.
 * Exits 0 once every change is written at the first poll after its key event.
 * Exits 1 on a late change, a re-send ahead of a waiting change or at deadline.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
#define RELAY_POLL_MS 20
#define CHANGES_EXPECTED 3
#define REPORT_ID_OFFSET 0

BUILD_ASSERT(CHANGES_EXPECTED * sizeof(struct zmk_hid_keyboard_report) ==
                 CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "the burst must fill one ACK whole");

static uint32_t polls_done;
static uint32_t change_event_poll[CHANGES_EXPECTED];
static size_t changes_seen;
static size_t changes_written;
static struct zmk_hid_keyboard_report last_keyboard = {.report_id = ZMK_HID_REPORT_ID_KEYBOARD};

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

ZMK_LISTENER(central_hid_relay_burst_room_test, keycode_listener);
ZMK_SUBSCRIPTION(central_hid_relay_burst_room_test, zmk_keycode_state_changed);

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

static void check_keyboard(const uint8_t *report) {
    if (memcmp(&last_keyboard, report, sizeof(last_keyboard)) == 0) {
        if (changes_written < changes_seen) {
            printk("FAIL: re-send written at poll %u while change %u of %u waits\n",
                   (unsigned int)polls_done, (unsigned int)(changes_written + 1),
                   (unsigned int)CHANGES_EXPECTED);
            exit(1);
        }
        return;
    }
    memcpy(&last_keyboard, report, sizeof(last_keyboard));
    uint32_t due_poll = change_event_poll[changes_written] + 1;
    if (changes_written == changes_seen || polls_done != due_poll) {
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
        uint8_t report_id = payload->data[offset + REPORT_ID_OFFSET];
        size_t length = report_length(report_id);
        if (length == 0 || offset + length > payload->length) {
            printk("FAIL: reply at poll %u does not split into whole reports\n",
                   (unsigned int)polls_done);
            exit(1);
        }
        if (report_id == ZMK_HID_REPORT_ID_KEYBOARD) {
            check_keyboard(&payload->data[offset]);
        }
        offset += length;
    }
    return 0;
}

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    polls_done++;
    esb_link_role_rx_done((uint8_t)BIT_MASK(esb_link_pipe_count));
    k_work_reschedule(&relay_poll_work, K_MSEC(RELAY_POLL_MS));
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: %u of %u burst changes written by deadline\n", (unsigned int)changes_written,
           (unsigned int)CHANGES_EXPECTED);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(RELAY_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
