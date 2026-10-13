// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a peripheral whose central matches its peripheral list and hop pool.
 * Exits 0 once a matching mask update and beacon leave the config mismatch flag clear.
 * Exits 1 on a mismatch flag.
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

static const struct esb_mask_update matching_mask = {.tag = ESB_MASK_UPDATE_TAG};
static const struct esb_beacon matching_beacon = {.tag = ESB_BEACON_TAG};

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static int test_radio_init(void) {
    (void)hop_consume_rx(SELF_PIPE,
                         (const uint8_t *)&matching_mask,
                         sizeof(matching_mask),
                         0);
    (void)hop_consume_rx(SELF_PIPE,
                         (const uint8_t *)&matching_beacon,
                         sizeof(matching_beacon),
                         0);
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    mock_check(!status.config_mismatch, "matching lengths leave the mismatch flag clear");
    printk("PASS: all %u matching length checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
