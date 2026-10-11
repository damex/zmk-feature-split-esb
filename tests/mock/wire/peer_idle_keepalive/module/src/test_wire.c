// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap on an idle wire peer's UART.
 * Exits 0 once idle keepalives hold the ESB idle-keepalive-ms cadence and report idle.
 * Exits 1 on a longer gap, a non-idle state or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "esb_keepalive.h"
#include "mock_wire.h"

#define TX_POLL_MS 4
#define GAPS_CHECKED 8
#define VERDICT_DEADLINE_MS 3000

static const uint32_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);

static uint32_t last_keepalive_ms;
static bool keepalive_seen;
static size_t gaps_checked;

static void on_tx_frame(const uint8_t *payload, size_t length) {
    if (!esb_keepalive_matches(payload, (uint8_t)length)) {
        return;
    }
    uint8_t state = esb_keepalive_state(payload);
    if (state != ESB_KEEPALIVE_IDLE) {
        printk("FAIL: wire peer keepalive reports state 0x%02x, expected idle 0x%02x\n", state,
               ESB_KEEPALIVE_IDLE);
        exit(1);
    }
    uint32_t now_ms = k_uptime_get_32();
    if (keepalive_seen) {
        uint32_t gap_ms = now_ms - last_keepalive_ms;
        if (gap_ms > idle_keepalive_ms + TX_POLL_MS) {
            printk("FAIL: idle keepalive gap %u ms, idle-keepalive-ms is %u\n",
                   (unsigned int)gap_ms, (unsigned int)idle_keepalive_ms);
            exit(1);
        }
        gaps_checked++;
        if (gaps_checked == GAPS_CHECKED) {
            printk("PASS: %u idle keepalive gaps, each within %u ms, all reporting idle\n",
                   (unsigned int)GAPS_CHECKED, (unsigned int)idle_keepalive_ms);
            exit(0);
        }
    }
    last_keepalive_ms = now_ms;
    keepalive_seen = true;
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
    printk("FAIL: %u of %u idle keepalive gaps by deadline\n", (unsigned int)gaps_checked,
           (unsigned int)GAPS_CHECKED);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_wire_init(void) {
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_wire_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
