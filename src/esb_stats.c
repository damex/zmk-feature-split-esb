// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Link counters, logged and cleared once a second, lines by role. */
#define DT_DRV_COMPAT zmk_split_esb

#include "esb_stats.h"

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define PIPE_COUNT DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals))
#define STATS_PERIOD_MS 1000

static atomic_t counts[ESB_STAT_COUNT];
static atomic_t received_counts[PIPE_COUNT];
static atomic_t data_counts[PIPE_COUNT];

void esb_stat_add(enum esb_stat stat, uint32_t amount) {
    __ASSERT_NO_MSG(stat < ESB_STAT_COUNT);
    (void)atomic_add(&counts[stat], (atomic_val_t)amount);
}

void esb_stat_note_rx(uint8_t pipe, bool data) {
    if (pipe >= PIPE_COUNT) {
        return;
    }
    (void)atomic_inc(&received_counts[pipe]);
    if (data) {
        (void)atomic_inc(&data_counts[pipe]);
    }
}

static uint32_t take_count(enum esb_stat stat) {
    return (uint32_t)atomic_set(&counts[stat], 0);
}

static void log_central(void) {
    for (uint8_t pipe = 0; pipe < PIPE_COUNT; pipe++) {
        uint32_t received = (uint32_t)atomic_set(&received_counts[pipe], 0);
        uint32_t data = (uint32_t)atomic_set(&data_counts[pipe], 0);
        LOG_DBG("stats: pipe %u rx %u data %u", pipe, received, data);
    }
    uint32_t written = take_count(ESB_STAT_REPLY_WRITTEN);
    uint32_t refused = take_count(ESB_STAT_REPLY_REFUSED);
    LOG_DBG("stats: replies written %u refused %u", written, refused);
}

static void log_peripheral(void) {
    uint32_t sent = take_count(ESB_STAT_SENT);
    uint32_t refused = take_count(ESB_STAT_SEND_REFUSED);
    uint32_t noack = take_count(ESB_STAT_NOACK_SENT);
    uint32_t ok = take_count(ESB_STAT_TX_OK);
    uint32_t retries = take_count(ESB_STAT_TX_RETRIES);
    uint32_t failed = take_count(ESB_STAT_TX_FAILED);
    uint32_t flushed = take_count(ESB_STAT_TX_FLUSHED);
    LOG_DBG("stats: sent %u refused %u noack %u, tx ok %u retries %u failed %u flushed %u",
            sent,
            refused,
            noack,
            ok,
            retries,
            failed,
            flushed);
}

static void log_relay(void) {
    uint32_t polls = take_count(ESB_STAT_RELAY_POLLS);
    uint32_t new_motion = take_count(ESB_STAT_RELAY_NEW_MOTION);
    uint32_t usb_took = take_count(ESB_STAT_RELAY_USB_TOOK);
    uint32_t usb_refused = take_count(ESB_STAT_RELAY_USB_REFUSED);
    LOG_DBG("stats: relay polls %u, new motion %u, usb took %u refused %u",
            polls,
            new_motion,
            usb_took,
            usb_refused);
}

static void stats_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(stats_work, stats_work_fn);

static void stats_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
        log_central();
    } else {
        log_peripheral();
    }
    if (IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_HID_RELAY) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
        log_relay();
    }
    k_work_reschedule(&stats_work, K_MSEC(STATS_PERIOD_MS));
}

static int stats_init(void) {
    k_work_reschedule(&stats_work, K_MSEC(STATS_PERIOD_MS));
    return 0;
}
SYS_INIT(stats_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
