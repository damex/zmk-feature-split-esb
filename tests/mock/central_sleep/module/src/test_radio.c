// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c and esb_sleep.c on a single-channel central.
 * Exits 0 once sleep stops RX, flushes TX and releases HFXO,
 * and waking after a failed poweroff restarts RX with HFXO held.
 * Exits 1 on a radio left running asleep or a link that stays down.
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

#include "mock.h"
#include "mock_esb.h"
#include "mock_hfclk.h"

#define SLEEP_DELAY_MS 100
#define SETTLE_MS 100

static size_t rx_starts_at_sleep;

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static void raise_activity(enum zmk_activity_state state) {
    raise_zmk_activity_state_changed((struct zmk_activity_state_changed){.state = state});
}

static void awake_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(mock_esb_rx_start_count() == rx_starts_at_sleep + 1,
               "waking after a failed poweroff restarts RX");
    mock_check(mock_hfclk_held(), "waking after a failed poweroff holds HFXO again");
    printk("PASS: all %u sleep checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(awake_work, awake_fn);

static void sleep_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(mock_esb_rx_start_count() > 0, "RX running before sleep");
    rx_starts_at_sleep = mock_esb_rx_start_count();
    size_t rx_stops_before = mock_esb_rx_stop_count();
    size_t flushes_before = mock_esb_flush_count();
    raise_activity(ZMK_ACTIVITY_SLEEP);
    mock_check(mock_esb_rx_stop_count() == rx_stops_before + 1, "sleep stops RX");
    mock_check(mock_esb_flush_count() == flushes_before + 1, "sleep flushes TX");
    mock_check(!mock_hfclk_held(), "sleep releases HFXO");
    raise_activity(ZMK_ACTIVITY_ACTIVE);
    k_work_reschedule(&awake_work, K_MSEC(SETTLE_MS));
}
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_fn);

static int test_radio_init(void) {
    k_work_reschedule(&sleep_work, K_MSEC(SLEEP_DELAY_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
