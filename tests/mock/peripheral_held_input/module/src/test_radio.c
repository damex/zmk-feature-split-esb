// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a peripheral with a split input device.
 * Exits 0 once keepalives list a held button while it is down and drop it after release.
 * Exits 1 on a wrong held list or at deadline.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "hop.h"

#define INPUT_REG DT_REG_ADDR(DT_NODELABEL(split_input))
#define VERDICT_DEADLINE_MS 1500

static bool held_seen;

static void check_held_key(const uint8_t *keepalive) {
    struct esb_keepalive_held_key key = esb_keepalive_held_key_at(keepalive, 0);
    if (esb_keepalive_held_count(keepalive) != 1 || key.reg != INPUT_REG ||
        key.code != INPUT_BTN_0) {
        printk("FAIL: keepalive holds %u keys, first reg %u code 0x%03x, expected reg %u code "
               "0x%03x\n",
               esb_keepalive_held_count(keepalive), key.reg, key.code, (unsigned int)INPUT_REG,
               INPUT_BTN_0);
        exit(1);
    }
}

static void check_keepalive(const uint8_t *keepalive) {
    uint8_t held_count = esb_keepalive_held_count(keepalive);
    if (!held_seen) {
        if (held_count == 0) {
            return;
        }
        check_held_key(keepalive);
        held_seen = true;
        return;
    }
    if (held_count != 0) {
        check_held_key(keepalive);
        return;
    }
    printk("PASS: keepalives held button 0 while down and dropped it after release\n");
    exit(0);
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        check_keepalive(payload->data);
    }
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (held_seen) {
        printk("FAIL: keepalives kept the button after its release\n");
    } else {
        printk("FAIL: no keepalive listed the held button\n");
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
