// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay USB sink on a relay-role peripheral. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static const struct device *hid_dev;
static K_SEM_DEFINE(hid_tx_sem, 1, 1);

static void in_ready_cb(const struct device *dev) {
    ARG_UNUSED(dev);
    k_sem_give(&hid_tx_sem);
}

static const struct hid_ops ops = {
    .int_in_ready = in_ready_cb,
};

#define RELAY_REPORT_ID_OFFSET 0

struct relay_report {
    uint8_t report_id;
    size_t length;
};

static const struct relay_report relay_reports[] = {
    {.report_id = ZMK_HID_REPORT_ID_KEYBOARD, .length = sizeof(struct zmk_hid_keyboard_report)},
    {.report_id = ZMK_HID_REPORT_ID_CONSUMER, .length = sizeof(struct zmk_hid_consumer_report)},
};

static bool relay_report_valid(const uint8_t *bytes, size_t length) {
    if (length <= RELAY_REPORT_ID_OFFSET) {
        return false;
    }
    for (size_t index = 0; index < ARRAY_SIZE(relay_reports); index++) {
        if (relay_reports[index].report_id == bytes[RELAY_REPORT_ID_OFFSET]) {
            return relay_reports[index].length == length;
        }
    }
    return false;
}

static void relay_to_usb(const uint8_t *bytes, size_t length) {
    if (hid_dev == NULL) {
        return;
    }
    if (!relay_report_valid(bytes, length)) {
        LOG_WRN("dropping non-report payload, %u bytes", (unsigned int)length);
        return;
    }
    if (k_sem_take(&hid_tx_sem, K_MSEC(10)) != 0) {
        return;
    }
    int err = hid_int_ep_write(hid_dev, bytes, length, NULL);
    if (err != 0) {
        k_sem_give(&hid_tx_sem);
        LOG_WRN("hid write failed (%d)", err);
    }
}

static int hid_relay_usb_init(void) {
    hid_dev = device_get_binding("HID_0");
    if (hid_dev == NULL) {
        LOG_ERR("HID_0 device not found");
        return -ENODEV;
    }
    usb_hid_register_device(hid_dev, zmk_hid_report_desc, sizeof(zmk_hid_report_desc), &ops);
    usb_hid_init(hid_dev);
    return zmk_split_esb_hid_relay_register(relay_to_usb);
}
SYS_INIT(hid_relay_usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
