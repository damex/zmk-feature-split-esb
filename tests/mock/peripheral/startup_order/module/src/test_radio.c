// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral booting through ZMK's transport enable.
 * Exits 0 once keepalives go out only after esb_init and the PTX never starts RX.
 * Exits 1 on a transmit or other ESB call before esb_init, an RX start, or no keepalive.
 */
#include <stddef.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "mock.h"
#include "mock_esb.h"

#define VERDICT_DELAY_MS 200

static size_t keepalives;

int esb_write_payload(const struct esb_payload *payload) {
    if (!mock_esb_initialized()) {
        printk("FAIL: peripheral transmitted before esb_init\n");
        exit(1);
    }
    if (esb_keepalive_matches(payload->data, payload->length)) {
        keepalives++;
    }
    return 0;
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(mock_esb_calls_before_init() == 0, "no ESB call before esb_init through boot");
    mock_check(mock_esb_rx_start_count() == 0, "PTX never starts RX");
    mock_check(keepalives > 0, "keepalives go out after boot");
    printk("PASS: %u keepalives after esb_init, no early ESB call, no RX start\n",
           (unsigned int)keepalives);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_DELAY_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
