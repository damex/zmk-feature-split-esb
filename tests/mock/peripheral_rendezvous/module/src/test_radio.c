// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a peripheral whose every transmit fails.
 * Exits 0 once retunes sweep the pool per dwell, then camp each anchor in turn, still searching.
 * Exits 1 on a wrong, early or late retune, a link reported found, or at deadline.
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
#include "mock_esb.h"

#define POOL_CHANNEL(index) DT_INST_PROP_BY_IDX(0, hop_channels, index)
#define ANCHOR_CHANNEL(slot) DT_INST_PROP_BY_IDX(0, hop_anchors, slot)
#define DWELL_WINDOWS ESB_HOP_SWEEP_DWELL_WINDOWS
#define CAMP_WINDOWS (ESB_HOP_ANCHOR_DWELL_WINDOWS + 1)
#define LAST_RETUNE_WINDOW (ESB_HOP_SWEEP_WINDOWS + 2 * CAMP_WINDOWS)
#define VERDICT_DEADLINE_MS 3000

BUILD_ASSERT(HOP_COUNT == 4, "expected retunes walk a four-channel pool");
BUILD_ASSERT(ESB_HOP_ANCHOR_COUNT == 2, "expected retunes camp two anchors");

struct retune {
    uint32_t window;
    uint8_t channel;
};

static const struct retune expected[] = {
    {.window = 1 * DWELL_WINDOWS, .channel = POOL_CHANNEL(1)},
    {.window = 2 * DWELL_WINDOWS, .channel = POOL_CHANNEL(2)},
    {.window = 3 * DWELL_WINDOWS, .channel = POOL_CHANNEL(3)},
    {.window = 4 * DWELL_WINDOWS, .channel = POOL_CHANNEL(0)},
    {.window = 5 * DWELL_WINDOWS, .channel = POOL_CHANNEL(1)},
    {.window = ESB_HOP_SWEEP_WINDOWS, .channel = ANCHOR_CHANNEL(0)},
    {.window = ESB_HOP_SWEEP_WINDOWS + CAMP_WINDOWS, .channel = ANCHOR_CHANNEL(1)},
    {.window = LAST_RETUNE_WINDOW, .channel = ANCHOR_CHANNEL(0)},
};

static uint32_t windows;

static size_t retunes_due(uint32_t window) {
    size_t due = 0;
    for (size_t index = 0; index < ARRAY_SIZE(expected); index++) {
        if (expected[index].window <= window) {
            due++;
        }
    }
    return due;
}

static uint32_t channel_due(uint32_t window) {
    uint32_t channel = 0;
    for (size_t index = 0; index < ARRAY_SIZE(expected); index++) {
        if (expected[index].window <= window) {
            channel = expected[index].channel;
        }
    }
    return channel;
}

static void check_window(void) {
    windows++;
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    if (!status.searching) {
        printk("FAIL: window %u reports the link found\n", (unsigned int)windows);
        exit(1);
    }
    size_t retunes = mock_esb_channel_set_count();
    if (retunes != retunes_due(windows)) {
        printk("FAIL: window %u after %u retunes, expected %u\n", (unsigned int)windows,
               (unsigned int)retunes, (unsigned int)retunes_due(windows));
        exit(1);
    }
    uint32_t channel = mock_esb_channel();
    if (channel != channel_due(windows)) {
        printk("FAIL: window %u on channel %u, expected %u\n", (unsigned int)windows,
               (unsigned int)channel, (unsigned int)channel_due(windows));
        exit(1);
    }
    if (windows > LAST_RETUNE_WINDOW) {
        printk("PASS: %u retunes swept the pool every %u windows, camped anchors every %u, "
               "searching throughout\n",
               (unsigned int)retunes, (unsigned int)DWELL_WINDOWS, (unsigned int)CAMP_WINDOWS);
        exit(0);
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        check_window();
    }
    hop_note_tx_failed();
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: rendezvous stopped at window %u of %u\n", (unsigned int)windows,
           (unsigned int)(LAST_RETUNE_WINDOW + 1));
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
