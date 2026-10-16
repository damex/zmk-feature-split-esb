// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap feeding a full duplex wire peer's UART.
 * Exits 0 once a beacon frame with an epoch and a roster signal leaves the wire peer's
 * channel, epoch, signal and delivery cost at 0.
 * Exits 1 on any of them nonzero, or at deadline.
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

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define CENTRAL_EPOCH 3
#define ROSTER_RSSI_DBM -50
#define INJECT_DELAY_MS 50
#define STATE_POLL_MS 4
#define VERDICT_DEADLINE_MS 500

static void inject_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct esb_beacon beacon = {
        .tag = ESB_BEACON_TAG,
        .epoch = CENTRAL_EPOCH,
        .hid_modifiers = MOD_LSFT,
        .peers = {
            [SELF_PIPE] = {.rssi_dbm = ROSTER_RSSI_DBM},
        },
    };
    mock_wire_rx_inject((const uint8_t *)&beacon, sizeof(beacon));
}
static K_WORK_DELAYABLE_DEFINE(inject_work, inject_fn);

static void check_status_zero(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    mock_check(status.channel == 0, "wire peer reports no channel");
    mock_check(status.epoch == 0, "wire peer reports no epoch");
    mock_check(status.rssi_dbm == 0, "wire peer reports no signal");
    mock_check(status.attempts_ewma_x10 == 0, "wire peer reports no delivery cost");
    printk("PASS: all %u wire peer radio field checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

static void state_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(state_poll_work, state_poll_fn);

static void state_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (zmk_split_esb_hid_modifiers() == MOD_LSFT) {
        check_status_zero();
    }
    k_work_reschedule(&state_poll_work, K_MSEC(STATE_POLL_MS));
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: wire peer never took the beacon frame\n");
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
