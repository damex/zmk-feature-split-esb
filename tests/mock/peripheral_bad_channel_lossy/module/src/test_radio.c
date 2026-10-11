// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a mouse with lossy X/Y streaming motion
 * over a bad channel.
 * Every acked packet needs BAD_ATTEMPTS attempts before its ACK.
 * Motion without ACK reports one attempt, as NCS does.
 * Exits 0 once the mouse steps off its channel.
 * Exits 1 when it stays there through the stream.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "mock_esb.h"

#define POINTER_DEVICE DEVICE_DT_GET(DT_NODELABEL(pointer_input))
#define MOTION_START_MS 50
#define MOTION_PERIOD_MS 4
#define BAD_ATTEMPTS 3
#define NOACK_ATTEMPTS 1
#define SETTLE_MS 100
#define SLOWEST_WINDOW_MS DT_INST_PROP(0, idle_keepalive_ms)
#define VERDICT_MS (SETTLE_MS + (DT_INST_PROP(0, hop_threshold) + 2) * SLOWEST_WINDOW_MS)

BUILD_ASSERT(DT_INST_NODE_HAS_PROP(0, lossy_codes), "lossy bad channel case sets lossy-codes");

static atomic_t packets;
static atomic_t noack_packets;
static atomic_t pending_acked;
static atomic_t pending_noack;
static atomic_t settled;
static atomic_t settled_channel;

static void report_successes(atomic_t *pending, uint32_t attempts) {
    for (atomic_val_t sent = atomic_set(pending, 0); sent > 0; sent--) {
        mock_esb_tx_success(attempts);
    }
}

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    report_successes(&pending_noack, NOACK_ATTEMPTS);
    report_successes(&pending_acked, BAD_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void check_channel(void) {
    uint32_t channel = (uint32_t)atomic_get(&settled_channel);
    if (atomic_get(&settled) == 0 || mock_esb_channel() == channel) {
        return;
    }
    printk("PASS: mouse left bad channel %u for %u after %u packets, %u without ACK\n",
           (unsigned int)channel,
           (unsigned int)mock_esb_channel(),
           (unsigned int)atomic_get(&packets),
           (unsigned int)atomic_get(&noack_packets));
    exit(0);
}

int esb_write_payload(const struct esb_payload *payload) {
    atomic_inc(&packets);
    if (payload->noack) {
        atomic_inc(&noack_packets);
        atomic_inc(&pending_noack);
    } else {
        atomic_inc(&pending_acked);
    }
    k_work_submit(&tx_success_work);
    check_channel();
    return 0;
}

static void motion_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(motion_work, motion_fn);

static void motion_fn(struct k_work *work) {
    ARG_UNUSED(work);
    input_report_rel(POINTER_DEVICE, INPUT_REL_X, 1, false, K_NO_WAIT);
    input_report_rel(POINTER_DEVICE, INPUT_REL_Y, 1, true, K_NO_WAIT);
    k_work_reschedule(&motion_work, K_MSEC(MOTION_PERIOD_MS));
}

static void settle_fn(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_set(&settled_channel, (atomic_val_t)mock_esb_channel());
    atomic_set(&settled, 1);
}
static K_WORK_DELAYABLE_DEFINE(settle_work, settle_fn);

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: mouse stayed on bad channel %u through %u packets, %u without ACK\n",
           (unsigned int)atomic_get(&settled_channel),
           (unsigned int)atomic_get(&packets),
           (unsigned int)atomic_get(&noack_packets));
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&motion_work, K_MSEC(MOTION_START_MS));
    k_work_reschedule(&settle_work, K_MSEC(SETTLE_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
