// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Relay dongle counters, logged and cleared once a second. */

#include "esb_hid_relay_stats.h"

#include <stdint.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define STATS_PERIOD_MS 1000

static atomic_t counts[ESB_HID_RELAY_STAT_COUNT];

void esb_hid_relay_stat_note(enum esb_hid_relay_stat stat) {
    __ASSERT_NO_MSG(stat < ESB_HID_RELAY_STAT_COUNT);
    (void)atomic_inc(&counts[stat]);
}

static uint32_t take_count(enum esb_hid_relay_stat stat) {
    return (uint32_t)atomic_set(&counts[stat], 0);
}

static void stats_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(stats_work, stats_work_fn);

static void stats_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t polls_sent = take_count(ESB_HID_RELAY_STAT_POLL_SENT);
    uint32_t polls_refused = take_count(ESB_HID_RELAY_STAT_POLL_REFUSED);
    uint32_t tx_acked = take_count(ESB_HID_RELAY_STAT_TX_ACKED);
    uint32_t tx_failed = take_count(ESB_HID_RELAY_STAT_TX_FAILED);
    uint32_t new_motion = take_count(ESB_HID_RELAY_STAT_NEW_MOTION);
    uint32_t usb_took = take_count(ESB_HID_RELAY_STAT_USB_TOOK);
    uint32_t usb_refused = take_count(ESB_HID_RELAY_STAT_USB_REFUSED);
    LOG_DBG("relay: polls %u refused %u, tx acked %u failed %u, new motion %u, "
            "usb took %u refused %u",
            polls_sent,
            polls_refused,
            tx_acked,
            tx_failed,
            new_motion,
            usb_took,
            usb_refused);
    k_work_reschedule(&stats_work, K_MSEC(STATS_PERIOD_MS));
}

static int stats_init(void) {
    k_work_reschedule(&stats_work, K_MSEC(STATS_PERIOD_MS));
    return 0;
}
SYS_INIT(stats_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
