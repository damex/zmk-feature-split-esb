// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake USB HID host and ESB link core under a real esb_hid_relay_usb.c on a relay dongle.
 * Host sets every lock indicator with an output report.
 * Exits 0 once the dongle sends those indicators up to the central.
 * Exits 1 when the dongle takes no output report, sends wrong indicators, or at deadline.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_ch9.h>

#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/hid.h>

#include "esb_hid_state.h"
#include "esb_link.h"
#include "hop.h"
#include "mock.h"

#define HID_SET_REPORT_OUTPUT 0x0200
#define DELIVER_DELAY_MS 50
#define VERDICT_DEADLINE_MS 1000

static const uint8_t host_indicators =
    HID_INDICATOR_NUM_LOCK |
    HID_INDICATOR_CAPS_LOCK |
    HID_INDICATOR_SCROLL_LOCK |
    HID_INDICATOR_COMPOSE |
    HID_INDICATOR_KANA;

static const struct hid_ops *hid_ops;

DEVICE_DEFINE(fake_hid, "HID_0", NULL, NULL, NULL, NULL, POST_KERNEL,
              CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);

void usb_hid_register_device(const struct device *dev, const uint8_t *desc, size_t size,
                             const struct hid_ops *op) {
    ARG_UNUSED(dev);
    ARG_UNUSED(desc);
    ARG_UNUSED(size);
    hid_ops = op;
}

int usb_hid_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

int hid_int_ep_write(const struct device *dev, const uint8_t *data, uint32_t data_len,
                     uint32_t *bytes_ret) {
    ARG_UNUSED(dev);
    ARG_UNUSED(data);
    ARG_UNUSED(data_len);
    ARG_UNUSED(bytes_ret);
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

int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    if (!esb_is_host_indicators(data, length)) {
        return 0;
    }
    uint8_t sent = data[offsetof(struct esb_host_indicators, indicators)];
    if (sent != host_indicators) {
        printk("FAIL: dongle sent indicators 0x%02x, host set 0x%02x\n", sent, host_indicators);
        exit(1);
    }
    mock_check(ack, "dongle asks an ACK for the host indicators");
    printk("PASS: dongle sent every host lock indicator up to the central\n");
    exit(0);
}

void hop_restore(void) {
}

uint8_t hop_link_cost_x10(void) {
    return 0;
}

static void deliver_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (hid_ops == NULL || hid_ops->set_report == NULL) {
        printk("FAIL: relay dongle takes no host output report\n");
        exit(1);
    }
    struct zmk_hid_led_report report = {
        .report_id = ZMK_HID_REPORT_ID_LEDS,
        .body = {.leds = host_indicators},
    };
    struct usb_setup_packet setup = {
        .bRequest = USB_HID_SET_REPORT,
        .wValue = HID_SET_REPORT_OUTPUT | ZMK_HID_REPORT_ID_LEDS,
        .wLength = sizeof(report),
    };
    int32_t length = sizeof(report);
    uint8_t *data = (uint8_t *)&report;
    mock_check(hid_ops->set_report(DEVICE_GET(fake_hid), &setup, &length, &data) == 0,
               "dongle accepts the host LED report");
}
static K_WORK_DELAYABLE_DEFINE(deliver_work, deliver_fn);

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: dongle never sent the host lock indicators up\n");
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&deliver_work, K_MSEC(DELIVER_DELAY_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
