// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a peripheral whose central lists one peripheral more.
 * Exits 0 once a beacon one peer longer than its own leaves the peer table and uplink signal
 * from the last matching beacon.
 * Exits 1 on any peer entry or uplink signal from the longer beacon.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "hop.h"
#include "hop_internal.h"
#include "mock.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define MATCHING_BATTERY 80
#define MATCHING_RSSI_DBM -50
#define LONGER_BATTERY 20
#define LONGER_RSSI_DBM -90
#define CENTRAL_BEACON_LENGTH (sizeof(struct esb_beacon) + sizeof(struct esb_beacon_peer))
#define SELF_PEER_OFFSET                                                                           \
    (offsetof(struct esb_beacon, peers) + SELF_PIPE * sizeof(struct esb_beacon_peer))

static const struct esb_beacon matching_beacon = {
    .tag = ESB_BEACON_TAG,
    .peers = {
        [SELF_PIPE] = {.battery = MATCHING_BATTERY, .rssi_dbm = MATCHING_RSSI_DBM},
    },
};
static const uint8_t longer_beacon[CENTRAL_BEACON_LENGTH] = {
    [offsetof(struct esb_beacon, tag)] = ESB_BEACON_TAG,
    [SELF_PEER_OFFSET + offsetof(struct esb_beacon_peer, battery)] = LONGER_BATTERY,
    [SELF_PEER_OFFSET + offsetof(struct esb_beacon_peer, rssi_dbm)] = (uint8_t)LONGER_RSSI_DBM,
};

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static void check_peer_table_kept(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    mock_check(zmk_split_esb_peer_battery(SELF_PIPE) == MATCHING_BATTERY,
               "longer beacon leaves the peer battery");
    mock_check(zmk_split_esb_peer_rssi_dbm(SELF_PIPE) == MATCHING_RSSI_DBM,
               "longer beacon leaves the peer signal");
    mock_check(status.rssi_dbm == MATCHING_RSSI_DBM, "longer beacon leaves the uplink signal");
}

static int test_radio_init(void) {
    (void)hop_consume_rx(SELF_PIPE,
                         (const uint8_t *)&matching_beacon,
                         sizeof(matching_beacon),
                         0);
    mock_check(zmk_split_esb_peer_battery(SELF_PIPE) == MATCHING_BATTERY,
               "matching beacon fills the peer table");
    (void)hop_consume_rx(SELF_PIPE, longer_beacon, sizeof(longer_beacon), 0);
    check_peer_table_kept();
    printk("PASS: all %u longer beacon peer table checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
