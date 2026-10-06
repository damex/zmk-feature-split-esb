// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a central booting through ZMK's transport enable.
 * Exits 0 once boot made no ESB call ahead of esb_init and left RX started.
 * Exits 1 on a radio call before esb_init.
 */
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "mock.h"
#include "mock_esb.h"

#define VERDICT_DELAY_MS 100

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    size_t early_calls = mock_esb_calls_before_init();
    if (early_calls != 0) {
        printk("FAIL: %u ESB calls reached the radio before esb_init\n", (unsigned int)early_calls);
        exit(1);
    }
    printk("PASS: no ESB call before esb_init through boot\n");
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_DELAY_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
