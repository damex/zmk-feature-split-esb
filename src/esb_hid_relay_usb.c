// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay USB sink on a relay-role peripheral. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

#include "esb_hid_state.h"
#include "esb_link.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define HID_REPORT_TYPE_MASK 0xFF00
#define HID_REPORT_ID_MASK 0x00FF
#define HID_REPORT_TYPE_OUTPUT 0x0200

static const struct device *hid_dev;
static K_SEM_DEFINE(hid_tx_sem, 1, 1);
static atomic_t host_indicators;

static void in_ready_cb(const struct device *dev) {
    ARG_UNUSED(dev);
    k_sem_give(&hid_tx_sem);
}

/* Off the USB control path, a radio send can wait for the crystal. */
static void host_indicators_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct esb_host_indicators packet = {
        .tag = ESB_HOST_INDICATORS_TAG,
        .indicators = (uint8_t)atomic_get(&host_indicators),
    };
    int error = esb_link_send((const uint8_t *)&packet, sizeof(packet), true);
    if (error != 0) {
        LOG_WRN("host indicators not sent (%d)", error);
    }
}
static K_WORK_DEFINE(host_indicators_work, host_indicators_work_fn);

static uint16_t setup_report_type(const struct usb_setup_packet *setup) {
    return setup->wValue & HID_REPORT_TYPE_MASK;
}

static uint8_t setup_report_id(const struct usb_setup_packet *setup) {
    return (uint8_t)(setup->wValue & HID_REPORT_ID_MASK);
}

static bool is_led_output_report(const struct usb_setup_packet *setup) {
    if (setup_report_type(setup) != HID_REPORT_TYPE_OUTPUT) {
        return false;
    }
    return setup_report_id(setup) == ZMK_HID_REPORT_ID_LEDS;
}

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    ARG_UNUSED(dev);
    if (!is_led_output_report(setup)) {
        return -ENOTSUP;
    }
    if (*len != sizeof(struct zmk_hid_led_report)) {
        return -EINVAL;
    }
    struct zmk_hid_led_report report = {0};
    memcpy(&report, *data, sizeof(report));
    atomic_set(&host_indicators, report.body.leds);
    k_work_submit(&host_indicators_work);
    return 0;
}

static const struct hid_ops ops = {
    .set_report = set_report_cb,
    .int_in_ready = in_ready_cb,
};

static int relay_to_usb(const uint8_t *bytes, size_t length) {
    if (hid_dev == NULL) {
        return -ENODEV;
    }
    if (k_sem_take(&hid_tx_sem, K_MSEC(10)) != 0) {
        return -EBUSY;
    }
    int err = hid_int_ep_write(hid_dev, bytes, length, NULL);
    if (err != 0) {
        k_sem_give(&hid_tx_sem);
        LOG_WRN("hid write failed (%d)", err);
    }
    return err;
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
