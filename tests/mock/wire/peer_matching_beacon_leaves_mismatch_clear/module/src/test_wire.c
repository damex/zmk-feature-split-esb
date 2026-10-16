// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap feeding a full duplex wire peer's UART.
 * Exits 0 once a matching beacon frame lands and leaves the config mismatch flag clear.
 * Exits 1 on a mismatch flag, or at deadline.
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

#define INJECT_DELAY_MS 50
#define STATE_POLL_MS 4
#define VERDICT_DEADLINE_MS 500

static void inject_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct esb_beacon beacon = {
        .tag = ESB_BEACON_TAG,
        .hid_modifiers = MOD_LSFT,
    };
    mock_wire_rx_inject((const uint8_t *)&beacon, sizeof(beacon));
}
static K_WORK_DELAYABLE_DEFINE(inject_work, inject_fn);

static void check_clear(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    mock_check(!status.config_mismatch, "matching beacon frame leaves the mismatch flag clear");
    printk("PASS: all %u wire peer matching beacon checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

static void state_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(state_poll_work, state_poll_fn);

static void state_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (zmk_split_esb_hid_modifiers() == MOD_LSFT) {
        check_clear();
    }
    k_work_reschedule(&state_poll_work, K_MSEC(STATE_POLL_MS));
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
