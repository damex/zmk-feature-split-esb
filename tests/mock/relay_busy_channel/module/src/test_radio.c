// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a relay dongle on a busy channel, every transmit acked.
 * One poll in BUSY_POLL_EVERY needs BUSY_ATTEMPTS attempts, the rest one.
 * Exits 0 once the dongle settled its link and stayed on its channel through the busy stretch.
 * Exits 1 when it leaves the channel, never settles its link, or sends no busy poll.
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

#include <esb.h>

#include "esb_hid_state.h"
#include "hop.h"
#include "mock_esb.h"

#define BUSY_POLL_EVERY 4
#define BUSY_ATTEMPTS 3
#define SETTLE_MS 100
#define SLOWEST_WINDOW_MS DT_INST_PROP(0, idle_keepalive_ms)
#define VERDICT_MS (SETTLE_MS + (DT_INST_PROP(0, hop_threshold) + 2) * SLOWEST_WINDOW_MS)

static size_t polls;
static size_t busy_polls;
static uint32_t settled_channel;
static atomic_t settled;

static void check_channel(void) {
    if (atomic_get(&settled) == 0 || mock_esb_channel() == settled_channel) {
        return;
    }
    printk("FAIL: dongle left channel %u for %u after %u polls, %u of them busy\n",
           (unsigned int)settled_channel, (unsigned int)mock_esb_channel(), (unsigned int)polls,
           (unsigned int)busy_polls);
    exit(1);
}

int esb_write_payload(const struct esb_payload *payload) {
    if (!esb_is_relay_poll(payload->data, payload->length)) {
        hop_note_tx_success(1);
        return 0;
    }
    polls++;
    uint8_t attempts = 1;
    if (polls % BUSY_POLL_EVERY == 0) {
        attempts = BUSY_ATTEMPTS;
        busy_polls++;
    }
    hop_note_tx_success(attempts);
    check_channel();
    return 0;
}

static void settle_fn(struct k_work *work) {
    ARG_UNUSED(work);
    settled_channel = mock_esb_channel();
    atomic_set(&settled, 1);
}
static K_WORK_DELAYABLE_DEFINE(settle_work, settle_fn);

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (busy_polls == 0) {
        printk("FAIL: dongle sent no busy poll in %u polls\n", (unsigned int)polls);
        exit(1);
    }
    if (!hop_link_acked()) {
        printk("FAIL: dongle never settled its link in %u polls\n", (unsigned int)polls);
        exit(1);
    }
    check_channel();
    printk("PASS: dongle stayed on channel %u through %u polls, %u of them busy\n",
           (unsigned int)settled_channel, (unsigned int)polls, (unsigned int)busy_polls);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    apply_hop_channel();
    hop_start();
    k_work_reschedule(&settle_work, K_MSEC(SETTLE_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
