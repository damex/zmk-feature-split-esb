// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a wire relay half.
 * Exits 0 once a beacon on the wire peer's pipe leaves the relay half reading its own entry.
 * Exits 1 otherwise.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "hop.h"
#include "hop_internal.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define PEER_PIPE DT_PROP(DT_CHOSEN(zmk_esb_wire_peer), pipe)
#define DELIVER_DELAY_MS 50
#define OWN_RSSI_DBM (-50)
#define STALE_PEER_RSSI_DBM (-80)

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static void deliver_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct esb_beacon beacon = {.tag = ESB_BEACON_TAG};
    beacon.peers[SELF_PIPE].rssi_dbm = OWN_RSSI_DBM;
    beacon.peers[PEER_PIPE].rssi_dbm = STALE_PEER_RSSI_DBM;
    (void)hop_consume_rx(PEER_PIPE, (const uint8_t *)&beacon, sizeof(beacon), 0);
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
