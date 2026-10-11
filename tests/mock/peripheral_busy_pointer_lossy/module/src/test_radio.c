// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a mouse with lossy X/Y streaming motion
 * over a busy channel.
 * One acked packet in BUSY_PACKET_EVERY needs BUSY_ATTEMPTS attempts, the rest one.
 * Motion without ACK reports one attempt, as NCS does.
 * Exits 0 once the mouse settled its link and stayed on its channel through the stream.
 * Exits 1 when it leaves the channel, never settles its link, or sends no busy packet.
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

#include "hop.h"
#include "mock_esb.h"

#define POINTER_DEVICE DEVICE_DT_GET(DT_NODELABEL(pointer_input))
#define MOTION_START_MS 50
#define MOTION_PERIOD_MS 4
#define BUSY_PACKET_EVERY 4
#define BUSY_ATTEMPTS 3
#define FIRST_TRY_ATTEMPTS 1
#define SETTLE_MS 100
#define SLOWEST_WINDOW_MS DT_INST_PROP(0, idle_keepalive_ms)
#define VERDICT_MS (SETTLE_MS + (DT_INST_PROP(0, hop_threshold) + 2) * SLOWEST_WINDOW_MS)

BUILD_ASSERT(DT_INST_NODE_HAS_PROP(0, lossy_codes), "lossy busy channel case sets lossy-codes");

static atomic_t packets;
static atomic_t acked_packets;
static atomic_t busy_packets;
static atomic_t noack_packets;
static atomic_t pending_first_try;
static atomic_t pending_busy;
static atomic_t settled;
static atomic_t settled_channel;

static void report_successes(atomic_t *pending, uint32_t attempts) {
    for (atomic_val_t sent = atomic_set(pending, 0); sent > 0; sent--) {
        mock_esb_tx_success(attempts);
    }
}

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    report_successes(&pending_first_try, FIRST_TRY_ATTEMPTS);
    report_successes(&pending_busy, BUSY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void check_channel(void) {
    uint32_t channel = (uint32_t)atomic_get(&settled_channel);
    if (atomic_get(&settled) == 0 || mock_esb_channel() == channel) {
        return;
    }
    printk("FAIL: mouse left channel %u for %u after %u packets, %u of them busy, "
           "%u without ACK\n",
           (unsigned int)channel, (unsigned int)mock_esb_channel(),
           (unsigned int)atomic_get(&packets), (unsigned int)atomic_get(&busy_packets),
           (unsigned int)atomic_get(&noack_packets));
    exit(1);
}

static void note_acked_packet(void) {
    atomic_val_t acked = atomic_inc(&acked_packets) + 1;
    if (acked % BUSY_PACKET_EVERY == 0) {
        atomic_inc(&busy_packets);
        atomic_inc(&pending_busy);
    } else {
        atomic_inc(&pending_first_try);
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    atomic_inc(&packets);
    if (payload->noack) {
        atomic_inc(&noack_packets);
        atomic_inc(&pending_first_try);
    } else {
        note_acked_packet();
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
    if (atomic_get(&busy_packets) == 0) {
        printk("FAIL: mouse sent no busy packet in %u packets\n",
               (unsigned int)atomic_get(&packets));
        exit(1);
    }
    if (!hop_link_acked()) {
        printk("FAIL: mouse never settled its link in %u packets\n",
               (unsigned int)atomic_get(&packets));
        exit(1);
    }
    check_channel();
    printk("PASS: mouse stayed on channel %u through %u packets, %u of them busy, "
           "%u without ACK\n",
           (unsigned int)atomic_get(&settled_channel), (unsigned int)atomic_get(&packets),
           (unsigned int)atomic_get(&busy_packets), (unsigned int)atomic_get(&noack_packets));
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&motion_work, K_MSEC(MOTION_START_MS));
    k_work_reschedule(&settle_work, K_MSEC(SETTLE_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
