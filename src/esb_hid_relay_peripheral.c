// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay, peripheral half: received reports to the registered sink. */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

#include "esb_hid_relay_peripheral.h"
#include "esb_hid_relay_pointer.h"
#include "esb_motion_total.h"
#include "hop_internal.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define RELAY_REPORT_ID_OFFSET 0

static atomic_ptr_t hid_relay_callback;

static struct zmk_hid_keyboard_report delivered_keyboard = {
    .report_id = ZMK_HID_REPORT_ID_KEYBOARD,
};
static struct zmk_hid_consumer_report delivered_consumer = {
    .report_id = ZMK_HID_REPORT_ID_CONSUMER,
};
static uint8_t delivered_buttons;
static struct esb_motion_track pointer_tracks[ESB_HID_RELAY_POINTER_AXES];
static uint32_t pointer_heard_ms;

struct relay_report {
    uint8_t report_id;
    size_t length;
    uint8_t *delivered;
    bool (*is_news)(const struct relay_report *report, const uint8_t *bytes);
};

static bool state_is_news(const struct relay_report *report, const uint8_t *bytes) {
    return memcmp(report->delivered, bytes, report->length) != 0;
}

static size_t pointer_axis_offset(size_t axis) {
    return ESB_HID_RELAY_POINTER_MOTION_OFFSET + axis * sizeof(int16_t);
}

static bool pointer_totals_to_deltas(const uint8_t *bytes, uint8_t *usb,
                                     struct esb_motion_track *tracks) {
    memcpy(usb, bytes, ESB_HID_RELAY_POINTER_LENGTH);
    bool moves = false;
    for (size_t axis = 0; axis < ESB_HID_RELAY_POINTER_AXES; axis++) {
        int16_t short_total = (int16_t)sys_get_le16(&bytes[pointer_axis_offset(axis)]);
        int32_t delta = 0;
        if (tracks[axis].valid) {
            (void)esb_motion_track_short_delta(&tracks[axis], short_total, &delta);
        } else {
            (void)esb_motion_track_delta(&tracks[axis], short_total, &delta);
        }
        sys_put_le16((uint16_t)(int16_t)delta, &usb[pointer_axis_offset(axis)]);
        moves |= delta != 0;
    }
    return moves;
}

static void forget_pointer_after_silence(void) {
    uint32_t now_ms = k_uptime_get_32();
    if (now_ms - pointer_heard_ms > ESB_HOP_LOSS_DETECT_MS) {
        for (size_t axis = 0; axis < ESB_HID_RELAY_POINTER_AXES; axis++) {
            pointer_tracks[axis].valid = false;
        }
    }
    pointer_heard_ms = now_ms;
}

/* A busy sink loses no motion, the next report carries it. */
static void deliver_pointer(zmk_split_esb_hid_relay_callback_t callback, const uint8_t *bytes) {
    forget_pointer_after_silence();
    uint8_t usb[ESB_HID_RELAY_POINTER_LENGTH];
    struct esb_motion_track tracks[ESB_HID_RELAY_POINTER_AXES];
    memcpy(tracks, pointer_tracks, sizeof(tracks));
    bool moves = pointer_totals_to_deltas(bytes, usb, tracks);
    uint8_t buttons = usb[ESB_HID_RELAY_POINTER_BUTTONS_OFFSET];
    if (moves || buttons != delivered_buttons) {
        if (callback(usb, sizeof(usb)) != 0) {
            return;
        }
        delivered_buttons = buttons;
    }
    memcpy(pointer_tracks, tracks, sizeof(tracks));
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
};

static const struct relay_report *relay_report_find(uint8_t report_id) {
    for (size_t index = 0; index < ARRAY_SIZE(relay_reports); index++) {
        if (relay_reports[index].report_id == report_id) {
            return &relay_reports[index];
        }
    }
    return NULL;
}

static size_t relay_report_length(uint8_t report_id, const struct relay_report *report) {
    if (report_id == ZMK_HID_REPORT_ID_MOUSE) {
        return ESB_HID_RELAY_POINTER_LENGTH;
    }
    if (report == NULL) {
        return 0;
    }
    return report->length;
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
        uint8_t report_id = bytes[offset + RELAY_REPORT_ID_OFFSET];
        const struct relay_report *report = relay_report_find(report_id);
        size_t report_length = relay_report_length(report_id, report);
        if (report_length == 0 || report_length > length - offset) {
            LOG_WRN("dropping relay payload, no whole report at offset %u", (unsigned int)offset);
            return;
        }
        if (report == NULL) {
            deliver_pointer(callback, &bytes[offset]);
        } else {
            deliver_report(callback, report, &bytes[offset]);
        }
        offset += report_length;
    }
}
