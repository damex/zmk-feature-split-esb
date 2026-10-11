// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c and esb_sleep.c on a peripheral going idle and back.
 * Exits 0 once HFXO stays held while active or transmitting,
 * drops between idle keepalives and comes back with activity.
 * Exits 1 on a keepalive sent without HFXO, a wrong hold or release, or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "mock.h"
#include "mock_esb.h"
#include "mock_hfclk.h"

#define FIRST_TRY_ATTEMPTS 1
#define ACTIVE_WINDOW 1
#define BUSY_WINDOW 2
#define QUIET_WINDOW 3
#define SETTLE_MS 24
#define BUSY_MS 40
#define VERDICT_DEADLINE_MS 2000

BUILD_ASSERT(BUSY_MS > SETTLE_MS, "busy check lands before the transmit completes");
BUILD_ASSERT(BUSY_MS + SETTLE_MS < DT_INST_PROP(0, idle_keepalive_ms),
             "release check after the busy transmit lands before the next idle keepalive");

static size_t windows;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void raise_activity(enum zmk_activity_state state) {
    raise_zmk_activity_state_changed((struct zmk_activity_state_changed){.state = state});
}

static bool link_searching(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    return status.searching;
}

static void awake_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(mock_hfclk_held(), "activity holds HFXO again");
    printk("PASS: all %u idle clock checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(awake_work, awake_fn);

static void idle_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(!mock_hfclk_held(), "going idle releases HFXO");
}
static K_WORK_DELAYABLE_DEFINE(idle_work, idle_fn);

static void busy_done_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(!mock_hfclk_held(), "HFXO released once the radio goes idle");
}
static K_WORK_DELAYABLE_DEFINE(busy_done_work, busy_done_fn);

static void busy_tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
    k_work_reschedule(&busy_done_work, K_MSEC(SETTLE_MS));
}
static K_WORK_DELAYABLE_DEFINE(busy_tx_success_work, busy_tx_success_fn);

static void window_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (windows == ACTIVE_WINDOW) {
        mock_check(mock_hfclk_held(), "HFXO stays held while active");
        raise_activity(ZMK_ACTIVITY_IDLE);
        k_work_reschedule(&idle_work, K_MSEC(SETTLE_MS));
    } else if (windows == BUSY_WINDOW) {
        mock_check(mock_hfclk_held(), "HFXO stays held while the radio is busy");
    } else if (windows == QUIET_WINDOW) {
        mock_check(!mock_hfclk_held(), "HFXO released after an idle keepalive");
        raise_activity(ZMK_ACTIVITY_ACTIVE);
        k_work_reschedule(&awake_work, K_MSEC(SETTLE_MS));
    }
}
static K_WORK_DELAYABLE_DEFINE(window_work, window_fn);

int esb_write_payload(const struct esb_payload *payload) {
    if (!esb_keepalive_matches(payload->data, payload->length) || link_searching()) {
        k_work_submit(&tx_success_work);
        return 0;
    }
    windows++;
    mock_check(mock_hfclk_held(), "keepalive transmits with HFXO held");
    if (windows == BUSY_WINDOW) {
        mock_esb_tx_begin();
        k_work_reschedule(&busy_tx_success_work, K_MSEC(BUSY_MS));
    } else {
        k_work_submit(&tx_success_work);
    }
    k_work_reschedule(&window_work, K_MSEC(SETTLE_MS));
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: idle clock stopped at window %u\n", (unsigned int)windows);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
