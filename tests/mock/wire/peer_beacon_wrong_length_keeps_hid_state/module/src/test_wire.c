// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap feeding a full duplex wire peer's UART.
 * Exits 0 once a beacon frame one peer longer than its own leaves modifiers and indicators
 * from the last matching beacon frame.
 * Exits 1 on any HID state from the longer frame, or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/modifiers.h>

#include <zmk_split_esb.h>

#include "hop_internal.h"
#include "mock.h"
#include "mock_wire.h"

#define MATCHING_INDICATORS 0x01
#define LONGER_INDICATORS 0x02
#define CENTRAL_BEACON_LENGTH (sizeof(struct esb_beacon) + sizeof(struct esb_beacon_peer))
#define INJECT_DELAY_MS 50
#define STATE_POLL_MS 4
#define SETTLE_MS 50
#define VERDICT_DEADLINE_MS 500

static const struct esb_beacon matching_beacon = {
    .tag = ESB_BEACON_TAG,
    .hid_modifiers = MOD_LCTL,
    .hid_indicators = MATCHING_INDICATORS,
};
static const uint8_t longer_beacon[CENTRAL_BEACON_LENGTH] = {
    [offsetof(struct esb_beacon, tag)] = ESB_BEACON_TAG,
    [offsetof(struct esb_beacon, hid_modifiers)] = MOD_LSFT,
    [offsetof(struct esb_beacon, hid_indicators)] = LONGER_INDICATORS,
};

static void check_kept_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(zmk_split_esb_hid_modifiers() == MOD_LCTL,
               "longer beacon frame leaves the modifiers");
    mock_check(zmk_split_esb_hid_indicators() == MATCHING_INDICATORS,
               "longer beacon frame leaves the indicators");
    printk("PASS: all %u wire peer HID state checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(check_kept_work, check_kept_fn);

static void inject_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_wire_rx_inject((const uint8_t *)&matching_beacon, sizeof(matching_beacon));
}
static K_WORK_DELAYABLE_DEFINE(inject_work, inject_fn);

static void state_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(state_poll_work, state_poll_fn);

static void state_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (zmk_split_esb_hid_modifiers() != MOD_LCTL) {
        k_work_reschedule(&state_poll_work, K_MSEC(STATE_POLL_MS));
        return;
    }
    mock_wire_rx_inject(longer_beacon, sizeof(longer_beacon));
    k_work_reschedule(&check_kept_work, K_MSEC(SETTLE_MS));
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: wire peer never took the matching beacon frame\n");
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_wire_init(void) {
    k_work_reschedule(&inject_work, K_MSEC(INJECT_DELAY_MS));
    k_work_reschedule(&state_poll_work, K_MSEC(STATE_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_wire_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
