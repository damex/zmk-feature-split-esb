// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a relay dongle, every transmit acked at once.
 * Exits 0 once the dongle polled with one-byte polls at the relay poll rate,
 * kept its keepalive off the poll rate and went silent after hop_stop.
 * Exits 1 on any other transmit, too few polls, keepalives at the poll rate,
 * or a transmit after hop_stop.
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
#include "hop.h"

#define RELAY_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define HOP_WINDOW_MS DT_INST_PROP(0, hop_window_ms)
#define WINDOW_START_MS 100
#define STOP_MS 500
#define VERDICT_MS 600
#define WINDOW_MS (STOP_MS - WINDOW_START_MS)
#define POLLS_MIN (WINDOW_MS / (2 * CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS))
#define KEEPALIVES_MAX (WINDOW_MS / HOP_WINDOW_MS + 1)

BUILD_ASSERT(2 * CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS < HOP_WINDOW_MS,
             "relay poll must outpace the hop window");

static size_t polls;
static size_t keepalives;
static size_t sent_after_stop;
static atomic_t stopped;

int esb_write_payload(const struct esb_payload *payload) {
    bool keepalive = esb_keepalive_matches(payload->data, payload->length);
    bool poll = esb_is_relay_poll(payload->data, payload->length);
    if (payload->pipe != RELAY_PIPE || (!keepalive && !poll)) {
        printk("FAIL: dongle sent %u bytes on pipe %u, neither a relay poll nor a keepalive\n",
               (unsigned int)payload->length, (unsigned int)payload->pipe);
        exit(1);
    }
    hop_note_tx_success(1);
    if (atomic_get(&stopped) != 0) {
        sent_after_stop++;
        return 0;
    }
    if (k_uptime_get() < WINDOW_START_MS) {
        return 0;
    }
    if (keepalive) {
        keepalives++;
    } else {
        polls++;
    }
    return 0;
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (polls < POLLS_MIN) {
        printk("FAIL: dongle sent %u relay polls in %u ms, expected at least %u\n",
               (unsigned int)polls, (unsigned int)WINDOW_MS, (unsigned int)POLLS_MIN);
        exit(1);
    }
    if (keepalives == 0 || keepalives > KEEPALIVES_MAX) {
        printk("FAIL: dongle sent %u keepalives in %u ms, the hop window allows 1 to %u\n",
               (unsigned int)keepalives, (unsigned int)WINDOW_MS, (unsigned int)KEEPALIVES_MAX);
        exit(1);
    }
    if (sent_after_stop != 0) {
        printk("FAIL: dongle sent %u packets after hop_stop\n", (unsigned int)sent_after_stop);
        exit(1);
    }
    printk("PASS: dongle sent %u one-byte polls and %u keepalives in %u ms, none after hop_stop\n",
           (unsigned int)polls, (unsigned int)keepalives, (unsigned int)WINDOW_MS);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static void stop_fn(struct k_work *work) {
    ARG_UNUSED(work);
    hop_stop();
    atomic_set(&stopped, 1);
}
static K_WORK_DELAYABLE_DEFINE(stop_work, stop_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&stop_work, K_MSEC(STOP_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
