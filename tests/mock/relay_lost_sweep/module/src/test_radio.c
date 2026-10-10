// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a relay dongle whose central is gone, every transmit fails.
 * Exits 0 once the dongle swept SWEEP_STEPS_MIN channels,
 * holding each for a central decision tick or longer.
 * Exits 1 on a shorter hold or too few sweep steps.
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

#include <esb.h>

#include "hop.h"
#include "mock_esb.h"

#define DECISION_TICK_MS DT_INST_PROP(0, idle_keepalive_ms)
#define SWEEP_STEPS_MIN 3
#define VERDICT_MS 2000

static uint32_t held_channel;
static int64_t held_since_ms;
static size_t sweep_steps;

static void check_sweep(void) {
    uint32_t channel = mock_esb_channel();
    if (channel == held_channel) {
        return;
    }
    int64_t now_ms = k_uptime_get();
    int64_t held_ms = now_ms - held_since_ms;
    if (held_ms < DECISION_TICK_MS) {
        printk("FAIL: dongle left channel %u after %u ms, a central decision tick is %u ms\n",
               (unsigned int)held_channel, (unsigned int)held_ms, (unsigned int)DECISION_TICK_MS);
        exit(1);
    }
    held_channel = channel;
    held_since_ms = now_ms;
    sweep_steps++;
}

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    hop_note_tx_failed();
    check_sweep();
    return 0;
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (sweep_steps < SWEEP_STEPS_MIN) {
        printk("FAIL: dongle swept %u channels in %u ms, expected at least %u\n",
               (unsigned int)sweep_steps, (unsigned int)VERDICT_MS,
               (unsigned int)SWEEP_STEPS_MIN);
        exit(1);
    }
    printk("PASS: dongle swept %u channels, holding each a central decision tick or longer\n",
           (unsigned int)sweep_steps);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    apply_hop_channel();
    held_channel = mock_esb_channel();
    held_since_ms = k_uptime_get();
    hop_start();
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
