// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a peripheral whose transmits land after retries.
 * Exits 0 once a mask update and a new-epoch beacon retune it to the masked epoch channel,
 * with the link cost back at the first-try baseline.
 * Exits 1 on a wrong retune, epoch or link cost, or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

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
#include "hop_policy.h"
#include "mock.h"
#include "mock_radio_peripheral.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define EPOCH 1
#define DROPPED_INDEX (EPOCH % HOP_COUNT)
#define ADOPTED_INDEX ((DROPPED_INDEX + 1) % HOP_COUNT)
#define TX_ATTEMPTS 3
#define WARMUP_WINDOWS 4
#define FIRST_TRY_COST_X10 10
#define VERDICT_DEADLINE_MS 1000

static const uint8_t pool_channels[] = DT_INST_PROP(0, hop_channels);
static uint32_t windows;

static void deliver_epoch(void) {
    struct esb_mask_update update = {.tag = ESB_MASK_UPDATE_TAG};
    for (size_t index = 0; index < HOP_COUNT; index++) {
        hop_policy_mask_set(update.mask, index, index != DROPPED_INDEX);
    }
    mock_check(hop_consume_rx(SELF_PIPE, (const uint8_t *)&update, sizeof(update), 0),
               "hop layer takes the mask update");
    const struct esb_beacon beacon = {.tag = ESB_BEACON_TAG, .epoch = EPOCH};
    mock_check(hop_consume_rx(SELF_PIPE, (const uint8_t *)&beacon, sizeof(beacon), 0),
               "hop layer takes the beacon");
}

static void check_adopted(const uint8_t *keepalive) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    mock_check(status.epoch == EPOCH, "peripheral adopts the beacon epoch");
    mock_check(mock_radio_peripheral_channel_set_count() == 1, "one retune for the new epoch");
    mock_check(mock_radio_peripheral_channel() == pool_channels[ADOPTED_INDEX],
               "retune skips the channel the staged mask dropped");
    mock_check(esb_keepalive_link_cost_x10(keepalive) == FIRST_TRY_COST_X10,
               "epoch adoption resets the link cost");
    printk("PASS: all %u epoch adoption checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

static void check_window(const uint8_t *keepalive) {
    windows++;
    if (windows < WARMUP_WINDOWS) {
        return;
    }
    if (windows == WARMUP_WINDOWS) {
        mock_check(esb_keepalive_link_cost_x10(keepalive) > FIRST_TRY_COST_X10,
                   "retried transmits raise the link cost");
        mock_check(mock_radio_peripheral_channel_set_count() == 0, "no retune before the beacon");
        deliver_epoch();
        return;
    }
    check_adopted(keepalive);
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        check_window(payload->data);
    }
    hop_note_tx_success(TX_ATTEMPTS);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: epoch adoption stopped at window %u\n", (unsigned int)windows);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
