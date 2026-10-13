// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a peripheral whose central pools more hop channels.
 * Exits 0 once a mask update one byte longer than its own flags a config mismatch.
 * Exits 1 on a flag before the mask update or none after it.
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
#define CENTRAL_MASK_LENGTH (ESB_MASK_UPDATE_LENGTH + 1)

static const uint8_t central_mask[CENTRAL_MASK_LENGTH] = {
    [offsetof(struct esb_mask_update, tag)] = ESB_MASK_UPDATE_TAG,
};

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static int test_radio_init(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    mock_check(!status.config_mismatch, "no mismatch before any mask update");
    (void)hop_consume_rx(SELF_PIPE, central_mask, sizeof(central_mask), 0);
    zmk_split_esb_get_status(&status);
    mock_check(status.config_mismatch, "longer mask update flags a config mismatch");
    printk("PASS: all %u longer mask update mismatch checks\n",
           (unsigned int)mock_checks_passed());
    exit(0);
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
