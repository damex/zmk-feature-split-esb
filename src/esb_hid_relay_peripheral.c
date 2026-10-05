// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay, peripheral half: received reports to the registered sink. */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

#include "esb_hid_relay_peripheral.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define RELAY_REPORT_ID_OFFSET 0

static atomic_ptr_t hid_relay_callback;

static struct zmk_hid_keyboard_report delivered_keyboard = {
    .report_id = ZMK_HID_REPORT_ID_KEYBOARD,
};
static struct zmk_hid_consumer_report delivered_consumer = {
    .report_id = ZMK_HID_REPORT_ID_CONSUMER,
};

struct relay_report {
    uint8_t report_id;
    size_t length;
    uint8_t *delivered;
};

static const struct relay_report relay_reports[] = {
    {
        .report_id = ZMK_HID_REPORT_ID_KEYBOARD,
        .length = sizeof(delivered_keyboard),
        .delivered = (uint8_t *)&delivered_keyboard,
    },
    {
        .report_id = ZMK_HID_REPORT_ID_CONSUMER,
        .length = sizeof(delivered_consumer),
        .delivered = (uint8_t *)&delivered_consumer,
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
    if (memcmp(report->delivered, bytes, report->length) == 0) {
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
