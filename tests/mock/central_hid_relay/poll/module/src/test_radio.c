// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on the central, with a relay dongle sending only one-byte polls.
 * Exits 0 once every poll stayed out of the event queue, the relay stayed heard
 * and the beacon refresh reached it.
 * Exits 1 on a poll handed up for queueing, a quiet relay, or no beacon.
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
#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"
#include "hop_internal.h"

#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define POLL_MS 4
#define VERDICT_MS 1100
#define QUIET_MAX_MS (2 * POLL_MS)

static size_t polls;
static size_t relay_beacons;

int esb_write_payload(const struct esb_payload *payload) {
    if (payload->pipe == RELAY_PIPE && esb_is_beacon(payload->data, payload->length)) {
        relay_beacons++;
    }
    return 0;
}

uint8_t esb_central_battery_level(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return ESB_KEEPALIVE_BATTERY_UNKNOWN;
}

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct esb_relay_poll poll = {.tag = ESB_RELAY_POLL_TAG};
    if (!hop_consume_rx(RELAY_PIPE, (const uint8_t *)&poll, sizeof(poll), 0)) {
        printk("FAIL: central hands relay poll %u up for its event queue\n",
               (unsigned int)(polls + 1));
        exit(1);
    }
    polls++;
    esb_link_role_rx_done((uint8_t)BIT(RELAY_PIPE));
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_MS));
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t quiet_ms = hop_pipe_quiet_ms(RELAY_PIPE);
    if (quiet_ms > QUIET_MAX_MS) {
        printk("FAIL: relay quiet for %u ms after %u polls\n", (unsigned int)quiet_ms,
               (unsigned int)polls);
        exit(1);
    }
    if (relay_beacons == 0) {
        printk("FAIL: beacon refresh skipped the relay over %u polls\n", (unsigned int)polls);
        exit(1);
    }
    printk("PASS: %u relay polls stayed out of the event queue, the relay got %u beacons\n",
           (unsigned int)polls, (unsigned int)relay_beacons);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
