// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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

static void relay_to_usb(const uint8_t *bytes, size_t length) {
    if (hid_dev == NULL) {
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
