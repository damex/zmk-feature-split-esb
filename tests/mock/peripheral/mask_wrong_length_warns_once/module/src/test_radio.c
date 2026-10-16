// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a peripheral whose central pools more hop channels.
 * Exits 0 once two mask updates one byte longer than its own log one refusal warning with
 * both lengths.
 * Exits 1 on no warning, a repeated one, or one without both lengths.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "hop.h"
#include "hop_internal.h"
#include "mock.h"
#include "mock_log.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define CENTRAL_MASK_LENGTH (ESB_MASK_UPDATE_LENGTH + 1)
#define REFUSAL_PREFIX "Refusing mask update length"
#define REFUSAL_LENGTH_MAX 96

static const uint8_t central_mask[CENTRAL_MASK_LENGTH] = {
    [offsetof(struct esb_mask_update, tag)] = ESB_MASK_UPDATE_TAG,
};

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static void check_refusal_lengths(void) {
    char refusal[REFUSAL_LENGTH_MAX];
    int written = snprintf(refusal,
                           sizeof(refusal),
                           REFUSAL_PREFIX " %u from central, expected %u for these hop-channels",
                           (unsigned int)CENTRAL_MASK_LENGTH,
                           (unsigned int)ESB_MASK_UPDATE_LENGTH);
    bool refusal_fits = written > 0 && (size_t)written < sizeof(refusal);
    mock_check(refusal_fits, "refusal text fits its buffer");
    mock_check(mock_log_count(refusal) == 1, "refusal names both mask update lengths");
}

static int test_radio_init(void) {
    (void)hop_consume_rx(SELF_PIPE, central_mask, sizeof(central_mask), 0);
    (void)hop_consume_rx(SELF_PIPE, central_mask, sizeof(central_mask), 0);
    mock_check(mock_log_count(REFUSAL_PREFIX) == 1, "two longer mask updates log one refusal");
    check_refusal_lengths();
    printk("PASS: all %u longer mask update warning checks\n",
           (unsigned int)mock_checks_passed());
    exit(0);
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
