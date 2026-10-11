// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap on a wire peer's UART with a split input device.
 * Exits 0 once keepalives list a held button while it is down and drop it after release.
 * Exits 1 on a wrong held list or at deadline.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "esb_keepalive.h"
#include "mock_wire.h"

#define INPUT_REG DT_REG_ADDR(DT_NODELABEL(split_input))
#define TX_POLL_MS 4
#define VERDICT_DEADLINE_MS 1500

static bool held_seen;

static void check_held_key(const uint8_t *keepalive) {
    struct esb_keepalive_held_key key = esb_keepalive_held_key_at(keepalive, 0);
    if (esb_keepalive_held_count(keepalive) != 1 || key.reg != INPUT_REG ||
        key.code != INPUT_BTN_0) {
        printk("FAIL: wire keepalive holds %u keys, first reg %u code 0x%03x, expected reg %u "
               "code 0x%03x\n",
               esb_keepalive_held_count(keepalive), key.reg, key.code, (unsigned int)INPUT_REG,
               INPUT_BTN_0);
        exit(1);
    }
}

static void on_tx_frame(const uint8_t *payload, size_t length) {
    if (!esb_keepalive_matches(payload, (uint8_t)length)) {
        return;
    }
    uint8_t held_count = esb_keepalive_held_count(payload);
    if (!held_seen) {
        if (held_count == 0) {
            return;
        }
        check_held_key(payload);
        held_seen = true;
        return;
    }
    if (held_count != 0) {
        check_held_key(payload);
        return;
    }
    printk("PASS: wire keepalives held button 0 while down and dropped it after release\n");
    exit(0);
}

static void tx_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tx_poll_work, tx_poll_fn);

static void tx_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    (void)mock_wire_tx_drain(on_tx_frame);
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (held_seen) {
        printk("FAIL: wire keepalives kept the button after its release\n");
    } else {
        printk("FAIL: no wire keepalive listed the held button\n");
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_wire_init(void) {
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_wire_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
