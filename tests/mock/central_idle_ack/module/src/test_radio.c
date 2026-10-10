// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on the central, with two idle halves polling.
 * Exits 0 once their ACKs carried only beacons and mask updates.
 * Exits 1 on any other half ACK, more control than the decision ticks allow, or none.
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
#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"
#include "hop_internal.h"

#define POLL_MS 4
#define VERDICT_MS 1100
#define HALF_COUNT DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals))
#define CONTROL_KINDS 2
#define CONTROL_WRITES_MAX ((VERDICT_MS / DT_INST_PROP(0, idle_keepalive_ms) + 1) * CONTROL_KINDS)

static const uint8_t no_positions[ESB_KEEPALIVE_BITMAP_BYTES];
static size_t idle_polls;
static size_t control_writes[HALF_COUNT];

static bool is_control(const struct esb_payload *payload) {
    if (esb_is_beacon(payload->data, payload->length)) {
        return true;
    }
    return esb_is_mask_update(payload->data, payload->length);
}

int esb_write_payload(const struct esb_payload *payload) {
    if (payload->pipe >= HALF_COUNT || !is_control(payload)) {
        printk("FAIL: idle half ACK on pipe %u carries %u bytes, no beacon or mask update\n",
               (unsigned int)payload->pipe, (unsigned int)payload->length);
        exit(1);
    }
    control_writes[payload->pipe]++;
    return 0;
}

uint8_t esb_central_battery_level(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return ESB_KEEPALIVE_BATTERY_UNKNOWN;
}

static void halves_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(halves_poll_work, halves_poll_fn);

static void halves_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct esb_keepalive_snapshot snapshot = {
        .state = ESB_KEEPALIVE_IDLE,
        .battery_level = ESB_KEEPALIVE_BATTERY_UNKNOWN,
        .position_bitmap = no_positions,
    };
    uint8_t keepalive[ESB_KEEPALIVE_LENGTH(0, 0)];
    esb_keepalive_encode(keepalive, sizeof(keepalive), &snapshot);
    for (uint8_t pipe = 0; pipe < HALF_COUNT; pipe++) {
        (void)hop_consume_rx(pipe, keepalive, sizeof(keepalive), 0);
    }
    idle_polls++;
    esb_link_role_rx_done((uint8_t)BIT_MASK(HALF_COUNT));
    k_work_reschedule(&halves_poll_work, K_MSEC(POLL_MS));
}

static void check_half(uint8_t pipe) {
    if (control_writes[pipe] == 0) {
        printk("FAIL: no beacon reached the half on pipe %u in %u polls\n", (unsigned int)pipe,
               (unsigned int)idle_polls);
        exit(1);
    }
    if (control_writes[pipe] > CONTROL_WRITES_MAX) {
        printk("FAIL: %u control ACKs on pipe %u, decision ticks allow %u\n",
               (unsigned int)control_writes[pipe], (unsigned int)pipe,
               (unsigned int)CONTROL_WRITES_MAX);
        exit(1);
    }
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    for (uint8_t pipe = 0; pipe < HALF_COUNT; pipe++) {
        check_half(pipe);
    }
    printk("PASS: %u idle polls per half wrote only beacons and mask updates\n",
           (unsigned int)idle_polls);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&halves_poll_work, K_MSEC(POLL_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
