// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c and esb_sleep.c on a peripheral.
 * Exits 0 once sleep flushes TX, releases HFXO and stops keepalives,
 * and waking after a failed poweroff brings keepalives back.
 * Exits 1 on a keepalive while asleep, a link that stays down, or at deadline.
 */
#include <stddef.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "mock.h"
#include "mock_esb.h"
#include "mock_hfclk.h"

#define FIRST_TRY_ATTEMPTS 1
#define SLEEP_DELAY_MS 100
#define SETTLE_MS 100

static size_t keepalives;
static size_t keepalives_at_sleep;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        keepalives++;
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static void raise_activity(enum zmk_activity_state state) {
    raise_zmk_activity_state_changed((struct zmk_activity_state_changed){.state = state});
}

static void awake_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(mock_hfclk_held(), "waking after a failed poweroff holds HFXO again");
    mock_check(keepalives > keepalives_at_sleep, "keepalives resume after a failed poweroff");
    printk("PASS: all %u sleep checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(awake_work, awake_fn);

static void asleep_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(keepalives == keepalives_at_sleep, "no keepalive while asleep");
    raise_activity(ZMK_ACTIVITY_ACTIVE);
    k_work_reschedule(&awake_work, K_MSEC(SETTLE_MS));
}
static K_WORK_DELAYABLE_DEFINE(asleep_work, asleep_fn);

static void sleep_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(keepalives > 0, "keepalives flow before sleep");
    size_t flushes_before = mock_esb_flush_count();
    raise_activity(ZMK_ACTIVITY_SLEEP);
    mock_check(mock_esb_flush_count() == flushes_before + 1, "sleep flushes TX");
    mock_check(!mock_hfclk_held(), "sleep releases HFXO");
    keepalives_at_sleep = keepalives;
    k_work_reschedule(&asleep_work, K_MSEC(SETTLE_MS));
}
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_fn);

static int test_radio_init(void) {
    k_work_reschedule(&sleep_work, K_MSEC(SLEEP_DELAY_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
