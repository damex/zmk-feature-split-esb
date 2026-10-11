// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a mouse streaming motion over a bad channel.
 * Every packet needs BAD_ATTEMPTS attempts before its ACK.
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
#define SETTLE_MS 100
#define SLOWEST_WINDOW_MS DT_INST_PROP(0, idle_keepalive_ms)
#define VERDICT_MS (SETTLE_MS + (DT_INST_PROP(0, hop_threshold) + 2) * SLOWEST_WINDOW_MS)

static atomic_t packets;
static atomic_t settled;
static atomic_t settled_channel;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(BAD_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void check_channel(void) {
    uint32_t channel = (uint32_t)atomic_get(&settled_channel);
    if (atomic_get(&settled) == 0 || mock_esb_channel() == channel) {
        return;
    }
    printk("PASS: mouse left bad channel %u for %u after %u packets\n", (unsigned int)channel,
           (unsigned int)mock_esb_channel(), (unsigned int)atomic_get(&packets));
    exit(0);
}

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    atomic_inc(&packets);
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
    printk("FAIL: mouse stayed on bad channel %u through %u packets\n",
           (unsigned int)atomic_get(&settled_channel), (unsigned int)atomic_get(&packets));
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
