// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a half typing fast over a busy channel.
 * One packet in BUSY_PACKET_EVERY needs BUSY_ATTEMPTS attempts, the rest one, every packet acked.
 * Exits 0 once the half settled its link and stayed on its channel through the typing.
 * Exits 1 when it leaves the channel, never settles its link, or sends no busy packet.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/events/position_state_changed.h>

#include <esb.h>

#include "hop.h"
#include "mock_esb.h"

#define TYPED_POSITION 0
#define TYPING_START_MS 50
#define KEY_EVENT_PERIOD_MS 20
#define BUSY_PACKET_EVERY 4
#define BUSY_ATTEMPTS 3
#define FIRST_TRY_ATTEMPTS 1
#define SETTLE_MS 100
#define SLOWEST_WINDOW_MS DT_INST_PROP(0, idle_keepalive_ms)
#define VERDICT_MS (SETTLE_MS + (DT_INST_PROP(0, hop_threshold) + 2) * SLOWEST_WINDOW_MS)

static atomic_t packets;
static atomic_t busy_packets;
static atomic_t pending_attempts;
static atomic_t settled;
static atomic_t settled_channel;
static bool key_down;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success((uint32_t)atomic_set(&pending_attempts, 0));
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void raise_pending_attempts(atomic_val_t attempts) {
    atomic_val_t current = atomic_get(&pending_attempts);
    while (current < attempts && !atomic_cas(&pending_attempts, current, attempts)) {
        current = atomic_get(&pending_attempts);
    }
}

static void check_channel(void) {
    uint32_t channel = (uint32_t)atomic_get(&settled_channel);
    if (atomic_get(&settled) == 0 || mock_esb_channel() == channel) {
        return;
    }
    printk("FAIL: half left channel %u for %u after %u packets, %u of them busy\n",
           (unsigned int)channel, (unsigned int)mock_esb_channel(),
           (unsigned int)atomic_get(&packets), (unsigned int)atomic_get(&busy_packets));
    exit(1);
}

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    atomic_val_t sent = atomic_inc(&packets) + 1;
    atomic_val_t attempts = FIRST_TRY_ATTEMPTS;
    if (sent % BUSY_PACKET_EVERY == 0) {
        attempts = BUSY_ATTEMPTS;
        atomic_inc(&busy_packets);
    }
    raise_pending_attempts(attempts);
    k_work_submit(&tx_success_work);
    check_channel();
    return 0;
}

static void typing_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(typing_work, typing_fn);

static void typing_fn(struct k_work *work) {
    ARG_UNUSED(work);
    key_down = !key_down;
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .position = TYPED_POSITION,
        .state = key_down,
        .timestamp = k_uptime_get(),
    });
    k_work_reschedule(&typing_work, K_MSEC(KEY_EVENT_PERIOD_MS));
}

static void settle_fn(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_set(&settled_channel, (atomic_val_t)mock_esb_channel());
    atomic_set(&settled, 1);
}
static K_WORK_DELAYABLE_DEFINE(settle_work, settle_fn);

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (atomic_get(&busy_packets) == 0) {
        printk("FAIL: half sent no busy packet in %u packets\n",
               (unsigned int)atomic_get(&packets));
        exit(1);
    }
    if (!hop_link_acked()) {
        printk("FAIL: half never settled its link in %u packets\n",
               (unsigned int)atomic_get(&packets));
        exit(1);
    }
    check_channel();
    printk("PASS: half stayed on channel %u through %u packets, %u of them busy\n",
           (unsigned int)atomic_get(&settled_channel), (unsigned int)atomic_get(&packets),
           (unsigned int)atomic_get(&busy_packets));
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&typing_work, K_MSEC(TYPING_START_MS));
    k_work_reschedule(&settle_work, K_MSEC(SETTLE_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
