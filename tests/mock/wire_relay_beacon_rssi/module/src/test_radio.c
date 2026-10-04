// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a wire relay half.
 * Exits 0 once a beacon on the wire peer's pipe leaves the relay half reading its own entry.
 * Exits 1 otherwise.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"
#include "hop_internal.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define PEER_PIPE DT_PROP(DT_CHOSEN(zmk_esb_wire_peer), pipe)
#define DELIVER_DELAY_MS 50
#define OWN_RSSI_DBM (-50)
#define STALE_PEER_RSSI_DBM (-80)

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

bool esb_is_idle(void) {
    return true;
}

int esb_flush_tx(void) {
    return 0;
}

int esb_set_tx_power(int8_t tx_output_power) {
    ARG_UNUSED(tx_output_power);
    return 0;
}

int esb_set_retransmit_delay(uint16_t delay) {
    ARG_UNUSED(delay);
    return 0;
}

int esb_set_retransmit_count(uint16_t count) {
    ARG_UNUSED(count);
    return 0;
}

int esb_set_rf_channel(uint32_t channel) {
    ARG_UNUSED(channel);
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    ARG_UNUSED(callback);
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    return 0;
}

int esb_link_hfclk_acquire(void) {
    return 0;
}

void esb_link_hfclk_release(void) {
}

void esb_link_mark_tx_event(void) {
}

uint32_t esb_link_tx_last_event_ms(void) {
    return 0;
}

static void deliver_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct esb_beacon beacon = {.tag = ESB_BEACON_TAG};
    beacon.peers[SELF_PIPE].rssi_dbm = OWN_RSSI_DBM;
    beacon.peers[PEER_PIPE].rssi_dbm = STALE_PEER_RSSI_DBM;
    if (!hop_consume_rx(PEER_PIPE, (const uint8_t *)&beacon, sizeof(beacon), 0)) {
        printk("FAIL: beacon on wire peer's pipe not consumed\n");
        exit(1);
    }
    int8_t rssi_dbm = zmk_split_esb_pipe_rssi_dbm(0);
    if (rssi_dbm != OWN_RSSI_DBM) {
        printk("FAIL: relay half reads %d dBm, own entry is %d dBm\n", rssi_dbm, OWN_RSSI_DBM);
        exit(1);
    }
    printk("PASS: beacon on wire peer's pipe left relay half on own entry, %d dBm\n", rssi_dbm);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(deliver_work, deliver_fn);

static int test_radio_init(void) {
    k_work_reschedule(&deliver_work, K_MSEC(DELIVER_DELAY_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
