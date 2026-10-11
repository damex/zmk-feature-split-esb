// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral with the link up.
 * Keepalive in flight keeps the radio busy across the tick that adopts a new epoch.
 * Busy radio refuses that retune, then the keepalive in flight fails.
 * Exits 0 once the TX restart goes out on the adopted epoch channel.
 * Exits 1 on a restart on the old channel, a missing setup step, or at deadline.
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

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "hop.h"
#include "hop_internal.h"
#include "mock.h"
#include "mock_esb.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define EPOCH 1
#define ADOPTED_INDEX (EPOCH % HOP_COUNT)
#define FIRST_TRY_ATTEMPTS 1
#define WARMUP_KEEPALIVES 4
#define VERDICT_DEADLINE_MS 1000

static const uint8_t pool_channels[] = DT_INST_PROP(0, hop_channels);
static size_t keepalives;
static bool keepalive_held;
static bool failure_raised;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void tx_failed_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_failed();
}
static K_WORK_DEFINE(tx_failed_work, tx_failed_fn);

static bool link_searching(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    return status.searching;
}

static bool epoch_adopted(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    return status.epoch == EPOCH;
}

static int check_restart(void) {
    mock_check(failure_raised, "TX restarts only after the keepalive in flight fails");
    mock_check(mock_esb_channel() == pool_channels[ADOPTED_INDEX],
               "TX restart goes out on the adopted epoch channel");
    printk("PASS: TX restart went out on channel %u, the busy radio had refused it\n",
           (unsigned int)mock_esb_channel());
    exit(0);
    return 0;
}

static void hold_keepalive(void) {
    mock_check(!link_searching(), "link up before the beacon");
    mock_check(mock_esb_channel() != pool_channels[ADOPTED_INDEX],
               "adopted epoch channel differs from the current one");
    keepalive_held = true;
    mock_esb_tx_begin();
    const struct esb_beacon beacon = {.tag = ESB_BEACON_TAG, .epoch = EPOCH};
    mock_check(hop_consume_rx(SELF_PIPE, (const uint8_t *)&beacon, sizeof(beacon), 0),
               "hop layer takes the beacon");
}

static void queue_behind_held(const struct esb_payload *payload) {
    if (failure_raised || !esb_keepalive_matches(payload->data, payload->length) ||
        !epoch_adopted()) {
        return;
    }
    mock_check(mock_esb_channel() != pool_channels[ADOPTED_INDEX],
               "busy radio refuses the epoch retune");
    failure_raised = true;
    k_work_submit(&tx_failed_work);
}

int esb_write_payload(const struct esb_payload *payload) {
    if (keepalive_held) {
        queue_behind_held(payload);
        return 0;
    }
    if (esb_keepalive_matches(payload->data, payload->length)) {
        keepalives++;
    }
    if (keepalives == WARMUP_KEEPALIVES) {
        hold_keepalive();
        return 0;
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: %u keepalives, held %u, failure raised %u, %u TX restarts by deadline\n",
           (unsigned int)keepalives, (unsigned int)keepalive_held, (unsigned int)failure_raised,
           (unsigned int)mock_esb_tx_start_count());
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    mock_esb_set_start_tx_handler(check_restart);
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
