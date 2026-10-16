// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a peripheral whose central lists one peripheral more.
 * Exits 0 once a beacon one peer longer than its own leaves modifiers and indicators
 * from the last matching beacon.
 * Exits 1 on any HID state from the longer beacon.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/modifiers.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "hop.h"
#include "hop_internal.h"
#include "mock.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define MATCHING_INDICATORS 0x01
#define LONGER_INDICATORS 0x02
#define CENTRAL_BEACON_LENGTH (sizeof(struct esb_beacon) + sizeof(struct esb_beacon_peer))

static const struct esb_beacon matching_beacon = {
    .tag = ESB_BEACON_TAG,
    .hid_modifiers = MOD_LCTL,
    .hid_indicators = MATCHING_INDICATORS,
};
static const uint8_t longer_beacon[CENTRAL_BEACON_LENGTH] = {
    [offsetof(struct esb_beacon, tag)] = ESB_BEACON_TAG,
    [offsetof(struct esb_beacon, hid_modifiers)] = MOD_LSFT,
    [offsetof(struct esb_beacon, hid_indicators)] = LONGER_INDICATORS,
};

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static void check_hid_state_kept(void) {
    mock_check(zmk_split_esb_hid_modifiers() == MOD_LCTL, "longer beacon leaves the modifiers");
    mock_check(zmk_split_esb_hid_indicators() == MATCHING_INDICATORS,
               "longer beacon leaves the indicators");
}

static int test_radio_init(void) {
    (void)hop_consume_rx(SELF_PIPE,
                         (const uint8_t *)&matching_beacon,
                         sizeof(matching_beacon),
                         0);
    mock_check(zmk_split_esb_hid_modifiers() == MOD_LCTL, "matching beacon syncs the modifiers");
    (void)hop_consume_rx(SELF_PIPE, longer_beacon, sizeof(longer_beacon), 0);
    check_hid_state_kept();
    printk("PASS: all %u longer beacon HID state checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
