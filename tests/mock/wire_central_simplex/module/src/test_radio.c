// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a central with a simplex wire peer.
 * Exits 0 once modifier changes and beacon refreshes pass with nothing on the wire.
 * Exits 1 on a wire transmit or without key activity.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#include <esb.h>

#include "central.h"
#include "esb_keepalive.h"
#include "hop.h"
#include "mock_wire.h"

#define TX_POLL_MS 4
#define HEARTBEAT_MS 100
#define VERDICT_MS 1200

static size_t keycode_changes;

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

uint8_t esb_central_battery_level(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return ESB_KEEPALIVE_BATTERY_UNKNOWN;
}

void central_ingest_packet(uint8_t pipe, const uint8_t *data, size_t length) {
    ARG_UNUSED(pipe);
    ARG_UNUSED(data);
    ARG_UNUSED(length);
}

static int keycode_listener(const zmk_event_t *event) {
    if (as_zmk_keycode_state_changed(event) != NULL) {
        keycode_changes++;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(wire_central_simplex_test, keycode_listener);
ZMK_SUBSCRIPTION(wire_central_simplex_test, zmk_keycode_state_changed);

static void tx_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tx_poll_work, tx_poll_fn);

static void tx_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    size_t tx_length = mock_wire_tx_drain(NULL);
    if (tx_length > 0) {
        printk("FAIL: central transmitted %u bytes on a simplex wire\n", (unsigned int)tx_length);
        exit(1);
    }
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
}

static void heartbeat_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_fn);

static void heartbeat_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_wire_rx_inject(NULL, 0);
    k_work_reschedule(&heartbeat_work, K_MSEC(HEARTBEAT_MS));
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (keycode_changes == 0) {
        printk("FAIL: no key activity reached the central\n");
        exit(1);
    }
    printk("PASS: %u keycode changes and beacon refreshes, nothing on the simplex wire\n",
           (unsigned int)keycode_changes);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
    k_work_reschedule(&heartbeat_work, K_NO_WAIT);
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
