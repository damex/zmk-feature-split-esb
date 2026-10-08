// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay, peripheral half: received reports to the registered sink. */

#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

#include "esb_hid_relay_peripheral.h"
#include "esb_hid_relay_pointer.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define RELAY_REPORT_ID_OFFSET 0

static atomic_ptr_t hid_relay_callback;

static struct zmk_hid_keyboard_report delivered_keyboard = {
    .report_id = ZMK_HID_REPORT_ID_KEYBOARD,
};
static struct zmk_hid_consumer_report delivered_consumer = {
    .report_id = ZMK_HID_REPORT_ID_CONSUMER,
};
static uint8_t delivered_pointer[ESB_HID_RELAY_POINTER_LENGTH];

struct relay_report {
    uint8_t report_id;
    size_t length;
    uint8_t *delivered;
    bool (*is_news)(const struct relay_report *report, const uint8_t *bytes);
};

static bool state_is_news(const struct relay_report *report, const uint8_t *bytes) {
    return memcmp(report->delivered, bytes, report->length) != 0;
}

static bool pointer_moves(const uint8_t *bytes) {
    for (size_t offset = ESB_HID_RELAY_POINTER_MOTION_OFFSET; offset < ESB_HID_RELAY_POINTER_LENGTH;
         offset++) {
        if (bytes[offset] != 0) {
            return true;
        }
    }
    return false;
}

/* Motion is a delta, so a repeat with motion moves again. */
static bool pointer_is_news(const struct relay_report *report, const uint8_t *bytes) {
    if (pointer_moves(bytes)) {
        return true;
    }
    return bytes[ESB_HID_RELAY_POINTER_BUTTONS_OFFSET] !=
           report->delivered[ESB_HID_RELAY_POINTER_BUTTONS_OFFSET];
}

static const struct relay_report relay_reports[] = {
    {
        .report_id = ZMK_HID_REPORT_ID_KEYBOARD,
        .length = sizeof(delivered_keyboard),
        .delivered = (uint8_t *)&delivered_keyboard,
        .is_news = state_is_news,
    },
    {
        .report_id = ZMK_HID_REPORT_ID_CONSUMER,
        .length = sizeof(delivered_consumer),
        .delivered = (uint8_t *)&delivered_consumer,
        .is_news = state_is_news,
    },
    {
        .report_id = ZMK_HID_REPORT_ID_MOUSE,
        .length = sizeof(delivered_pointer),
        .delivered = delivered_pointer,
        .is_news = pointer_is_news,
    },
};

static const struct relay_report *relay_report_find(uint8_t report_id) {
    for (size_t index = 0; index < ARRAY_SIZE(relay_reports); index++) {
        if (relay_reports[index].report_id == report_id) {
            return &relay_reports[index];
        }
    }
    return NULL;
}

int zmk_split_esb_hid_relay_register(zmk_split_esb_hid_relay_callback_t callback) {
    atomic_ptr_set(&hid_relay_callback, callback);
    return 0;
}

static void deliver_report(zmk_split_esb_hid_relay_callback_t callback,
                           const struct relay_report *report, const uint8_t *bytes) {
    if (!report->is_news(report, bytes)) {
        return;
    }
    if (callback(bytes, report->length) != 0) {
        return;
    }
    memcpy(report->delivered, bytes, report->length);
}

void esb_hid_relay_deliver(const uint8_t *bytes, size_t length) {
    zmk_split_esb_hid_relay_callback_t callback = atomic_ptr_get(&hid_relay_callback);
    if (callback == NULL) {
        return;
    }
    size_t offset = 0;
    while (offset < length) {
        const struct relay_report *report =
            relay_report_find(bytes[offset + RELAY_REPORT_ID_OFFSET]);
        if (report == NULL || report->length > length - offset) {
            LOG_WRN("dropping relay payload, no whole report at offset %u", (unsigned int)offset);
            return;
        }
        deliver_report(callback, report, &bytes[offset]);
        offset += report->length;
    }
}
