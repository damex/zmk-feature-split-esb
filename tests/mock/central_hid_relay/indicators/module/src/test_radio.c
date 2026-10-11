// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central with a relay dongle.
 * Exits 0 once every relayed host lock indicator lands in the central's HID indicators.
 * Exits 1 at deadline.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>

#include <esb.h>

#include "esb_hid_state.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define DELIVER_DELAY_MS 50
#define VERDICT_DEADLINE_MS 1000

static const uint8_t host_indicators =
    HID_INDICATOR_NUM_LOCK |
    HID_INDICATOR_CAPS_LOCK |
    HID_INDICATOR_SCROLL_LOCK |
    HID_INDICATOR_COMPOSE |
    HID_INDICATOR_KANA;

static esb_link_rx_callback_t rx_callback;

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    rx_callback = callback;
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

static int indicators_listener(const zmk_event_t *event) {
    const struct zmk_hid_indicators_changed *changed = as_zmk_hid_indicators_changed(event);
    if (changed == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (changed->indicators == host_indicators) {
        printk("PASS: every relayed host lock indicator reached the central's HID indicators\n");
        exit(0);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(central_hid_relay_indicators_test, indicators_listener);
ZMK_SUBSCRIPTION(central_hid_relay_indicators_test, zmk_hid_indicators_changed);

static void deliver_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct esb_host_indicators packet = {
        .tag = ESB_HOST_INDICATORS_TAG,
        .indicators = host_indicators,
    };
    rx_callback(RELAY_PIPE, (const uint8_t *)&packet, sizeof(packet));
}
static K_WORK_DELAYABLE_DEFINE(deliver_work, deliver_fn);

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: central HID indicators never took the relayed host lock indicators\n");
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&deliver_work, K_MSEC(DELIVER_DELAY_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
