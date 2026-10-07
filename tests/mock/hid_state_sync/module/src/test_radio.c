// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on the central, with a polling peripheral.
 * Exits 0 once beacon modifiers change in the expected order, 1 on a wrong change or deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/modifiers.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "esb_link_internal.h"
#include "hop.h"
#include "hop_internal.h"

#define POLL_MS 4
#define VERDICT_DEADLINE_MS 1000

static const uint8_t expected_modifiers[] = {
    MOD_LSFT,
    0,
};
static size_t next_change;
static uint8_t last_modifiers;

static void check_beacon(const struct esb_payload *payload) {
    struct esb_beacon beacon;
    memcpy(&beacon, payload->data, sizeof(beacon));
    if (beacon.hid_modifiers == last_modifiers) {
        return;
    }
    last_modifiers = beacon.hid_modifiers;
    if (beacon.hid_modifiers != expected_modifiers[next_change]) {
        printk("FAIL: step %u of %u, expected modifiers 0x%02x, got 0x%02x\n",
               (unsigned int)(next_change + 1), (unsigned int)ARRAY_SIZE(expected_modifiers),
               expected_modifiers[next_change], beacon.hid_modifiers);
        exit(1);
    }
    next_change++;
    if (next_change == ARRAY_SIZE(expected_modifiers)) {
        printk("PASS: beacons carried all %u modifier changes in order\n",
               (unsigned int)ARRAY_SIZE(expected_modifiers));
        exit(0);
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    if (payload->length == 0 || payload->length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    if (esb_is_beacon(payload->data, payload->length)) {
        check_beacon(payload);
    }
    return 0;
}

uint8_t esb_central_battery_level(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return ESB_KEEPALIVE_BATTERY_UNKNOWN;
}

static void peripheral_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(peripheral_poll_work, peripheral_poll_fn);

static void peripheral_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    static const uint8_t no_positions[ESB_KEEPALIVE_BITMAP_BYTES];
    const struct esb_keepalive_snapshot snapshot = {
        .state = ESB_KEEPALIVE_IDLE,
        .battery_level = ESB_KEEPALIVE_BATTERY_UNKNOWN,
        .position_bitmap = no_positions,
    };
    uint8_t keepalive[ESB_KEEPALIVE_LENGTH(0, 0)];
    esb_keepalive_encode(keepalive, sizeof(keepalive), &snapshot);
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        (void)hop_consume_rx(pipe, keepalive, sizeof(keepalive), 0);
    }
    esb_link_role_rx_done((uint8_t)BIT_MASK(esb_link_pipe_count));
    k_work_reschedule(&peripheral_poll_work, K_MSEC(POLL_MS));
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: beacons stopped at step %u of %u\n", (unsigned int)(next_change + 1),
           (unsigned int)ARRAY_SIZE(expected_modifiers));
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&peripheral_poll_work, K_MSEC(POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
