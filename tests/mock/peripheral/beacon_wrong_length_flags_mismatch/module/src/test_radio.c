// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a peripheral whose central lists one peripheral more.
 * Exits 0 once a beacon one peer longer than its own flags a config mismatch.
 * Exits 1 on a flag before the beacon or none after it.
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
#define CENTRAL_BEACON_LENGTH (sizeof(struct esb_beacon) + sizeof(struct esb_beacon_peer))

static const uint8_t central_beacon[CENTRAL_BEACON_LENGTH] = {
    [offsetof(struct esb_beacon, tag)] = ESB_BEACON_TAG,
};

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static int test_radio_init(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    mock_check(!status.config_mismatch, "no mismatch before any beacon");
    (void)hop_consume_rx(SELF_PIPE, central_beacon, sizeof(central_beacon), 0);
    zmk_split_esb_get_status(&status);
    mock_check(status.config_mismatch, "longer beacon flags a config mismatch");
    printk("PASS: all %u longer beacon mismatch checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
