// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a peripheral whose central lists one peripheral fewer.
 * Exits 0 once the hop layer takes a beacon one peer shorter than its own and keeps its epoch
 * and channel.
 * Exits 1 when the beacon reaches the command path, on an adopted epoch or a retune, or at
 * deadline.
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
#include "mock.h"
#include "mock_esb.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define BOOT_EPOCH 0
#define CENTRAL_EPOCH 1
#define CENTRAL_BEACON_LENGTH (sizeof(struct esb_beacon) - sizeof(struct esb_beacon_peer))
#define FIRST_TRY_ATTEMPTS 1
#define DELIVER_WINDOW 2
#define VERDICT_DEADLINE_MS 1000

BUILD_ASSERT(ESB_BEACON_PEER_COUNT > 1, "shorter beacon keeps at least one peer");

static const uint8_t central_beacon[CENTRAL_BEACON_LENGTH] = {
    [offsetof(struct esb_beacon, tag)] = ESB_BEACON_TAG,
    [offsetof(struct esb_beacon, epoch)] = CENTRAL_EPOCH,
};
static uint32_t windows;

static void deliver_beacon(void) {
    mock_check(hop_consume_rx(SELF_PIPE, central_beacon, sizeof(central_beacon), 0),
               "hop layer takes the shorter beacon");
}

static void check_kept(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    mock_check(status.epoch == BOOT_EPOCH, "peripheral keeps its epoch");
    mock_check(mock_esb_channel_set_count() == 0, "no retune for the shorter beacon");
    printk("PASS: all %u shorter beacon refusal checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

static void check_window(void) {
    windows++;
    if (windows < DELIVER_WINDOW) {
        return;
    }
    if (windows == DELIVER_WINDOW) {
        deliver_beacon();
        return;
    }
    check_kept();
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
    printk("FAIL: shorter beacon refusal stopped at window %u\n", (unsigned int)windows);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
