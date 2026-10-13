// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a peripheral whose central pools more hop channels.
 * Exits 0 once the hop layer takes a mask update one byte longer than its own and leaves it
 * unapplied, so the next epoch retunes to the channel that mask dropped.
 * Exits 1 on a refused mask update or beacon, a wrong retune, or at deadline.
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

#include "esb_keepalive.h"
#include "hop.h"
#include "hop_internal.h"
#include "hop_policy.h"
#include "mock.h"
#include "mock_esb.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define EPOCH 1
#define EPOCH_INDEX (EPOCH % HOP_COUNT)
#define CENTRAL_MASK_LENGTH (ESB_MASK_UPDATE_LENGTH + 1)
#define FIRST_TRY_ATTEMPTS 1
#define DELIVER_WINDOW 2
#define VERDICT_DEADLINE_MS 1000

static const uint8_t pool_channels[] = DT_INST_PROP(0, hop_channels);
static uint8_t central_mask[CENTRAL_MASK_LENGTH] = {
    [offsetof(struct esb_mask_update, tag)] = ESB_MASK_UPDATE_TAG,
};
static const struct esb_beacon matching_beacon = {.tag = ESB_BEACON_TAG, .epoch = EPOCH};
static uint32_t windows;

static void fill_central_mask(void) {
    uint8_t *mask = &central_mask[offsetof(struct esb_mask_update, mask)];
    for (size_t index = 0; index < HOP_COUNT; index++) {
        hop_policy_mask_set(mask, index, index != EPOCH_INDEX);
    }
}

static void deliver_epoch(void) {
    mock_check(hop_consume_rx(SELF_PIPE, central_mask, sizeof(central_mask), 0),
               "hop layer takes the longer mask update");
    mock_check(hop_consume_rx(SELF_PIPE,
                              (const uint8_t *)&matching_beacon,
                              sizeof(matching_beacon),
                              0),
               "hop layer takes the beacon");
}

static void check_unapplied(void) {
    mock_check(mock_esb_channel_set_count() == 1, "one retune for the new epoch");
    mock_check(mock_esb_channel() == pool_channels[EPOCH_INDEX],
               "longer mask update leaves the epoch channel active");
    printk("PASS: all %u longer mask update checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

static void check_window(void) {
    windows++;
    if (windows < DELIVER_WINDOW) {
        return;
    }
    if (windows == DELIVER_WINDOW) {
        deliver_epoch();
        return;
    }
    check_unapplied();
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        check_window();
    }
    hop_note_tx_success(FIRST_TRY_ATTEMPTS);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: longer mask update check stopped at window %u\n", (unsigned int)windows);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    fill_central_mask();
    hop_start();
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
