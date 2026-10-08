// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central with a relay dongle and a pointer input listener.
 * Pointer events arrive faster than the relay polls.
 * Exits 0 once the relay ACKs carry the summed motion, scroll and final buttons,
 * at most one pointer report each.
 * Exits 1 on an unknown report, two pointer reports in one ACK, or wrong totals at verdict.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/hid.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define EVENT_FIELDS 4
#define EVENT_TYPE 0
#define EVENT_CODE 1
#define EVENT_VALUE 2
#define REPORT_ID_OFFSET 0
#define POLL_START_MS 20
#define VERDICT_MS 300

struct pointer_totals {
    int32_t d_x;
    int32_t d_y;
    int32_t d_scroll_y;
    int32_t d_scroll_x;
    uint8_t buttons;
};

static const uint32_t pointer_events[] = DT_PROP(DT_NODELABEL(pointer_input), events);
static struct pointer_totals relayed;

static void add_button(struct pointer_totals *totals, uint32_t code, int32_t value) {
    uint8_t bit = (uint8_t)BIT(code - INPUT_BTN_0);
    if (value != 0) {
        totals->buttons |= bit;
    } else {
        totals->buttons &= (uint8_t)~bit;
    }
}

static void add_motion(struct pointer_totals *totals, uint32_t code, int32_t value) {
    switch (code) {
    case INPUT_REL_X:
        totals->d_x += value;
        break;
    case INPUT_REL_Y:
        totals->d_y += value;
        break;
    case INPUT_REL_WHEEL:
        totals->d_scroll_y += value;
        break;
    case INPUT_REL_HWHEEL:
        totals->d_scroll_x += value;
        break;
    default:
        /* the overlay sends no other relative code */
        break;
    }
}

static struct pointer_totals expected_totals(void) {
    struct pointer_totals totals = {0};
    for (size_t base = 0; base < ARRAY_SIZE(pointer_events); base += EVENT_FIELDS) {
        uint32_t code = pointer_events[base + EVENT_CODE];
        int32_t value = (int32_t)pointer_events[base + EVENT_VALUE];
        if (pointer_events[base + EVENT_TYPE] == INPUT_EV_KEY) {
            add_button(&totals, code, value);
        } else {
            add_motion(&totals, code, value);
        }
    }
    return totals;
}

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

static void add_pointer_report(const uint8_t *bytes) {
    struct zmk_hid_mouse_report report = {0};
    memcpy(&report, bytes, sizeof(report));
    relayed.d_x += report.body.d_x;
    relayed.d_y += report.body.d_y;
    relayed.d_scroll_y += report.body.d_scroll_y;
    relayed.d_scroll_x += report.body.d_scroll_x;
    relayed.buttons = report.body.buttons;
}

int esb_write_payload(const struct esb_payload *payload) {
    size_t offset = 0;
    size_t pointer_reports = 0;
    while (offset < payload->length) {
        uint8_t report_id = payload->data[offset + REPORT_ID_OFFSET];
        size_t length = report_length(report_id);
        if (length == 0 || offset + length > payload->length) {
            printk("FAIL: relay ACK of %u bytes is not whole HID reports\n",
                   (unsigned int)payload->length);
            exit(1);
        }
        if (report_id == ZMK_HID_REPORT_ID_MOUSE) {
            pointer_reports++;
            add_pointer_report(&payload->data[offset]);
        }
        offset += length;
    }
    if (pointer_reports > 1) {
        printk("FAIL: one relay ACK carries %u pointer reports\n", (unsigned int)pointer_reports);
        exit(1);
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

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    esb_link_role_rx_done((uint8_t)BIT(RELAY_PIPE));
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
}

static bool totals_match(const struct pointer_totals *expected) {
    if (relayed.d_x != expected->d_x || relayed.d_y != expected->d_y) {
        return false;
    }
    if (relayed.d_scroll_y != expected->d_scroll_y || relayed.d_scroll_x != expected->d_scroll_x) {
        return false;
    }
    return relayed.buttons == expected->buttons;
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct pointer_totals expected = expected_totals();
    if (!totals_match(&expected)) {
        printk("FAIL: relayed x %d y %d wheel %d hwheel %d buttons 0x%02x, "
               "expected x %d y %d wheel %d hwheel %d buttons 0x%02x\n",
               (int)relayed.d_x, (int)relayed.d_y, (int)relayed.d_scroll_y,
               (int)relayed.d_scroll_x, relayed.buttons, (int)expected.d_x, (int)expected.d_y,
               (int)expected.d_scroll_y, (int)expected.d_scroll_x, expected.buttons);
        exit(1);
    }
    printk("PASS: relay ACKs carried every pointer delta and the final buttons\n");
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_START_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
