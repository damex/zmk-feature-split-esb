// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap on an idle wire peer's UART.
 * Exits 0 once idle keepalives hold the ESB idle-keepalive-ms cadence.
 * Exits 1 on a longer gap or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "esb_keepalive.h"
#include "wire_frame.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define WIRE_UART DEVICE_DT_GET(DT_CHOSEN(zmk_esb_wire))
#define TX_POLL_MS 4
#define GAPS_CHECKED 8
#define VERDICT_DEADLINE_MS 3000

static const uint32_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);

static struct wire_frame_parser tx_parser;
static uint32_t last_keepalive_ms;
static bool keepalive_seen;
static size_t gaps_checked;

static void on_tx_frame(const uint8_t *payload, size_t length, void *user_data) {
    ARG_UNUSED(user_data);
    if (!esb_keepalive_matches(payload, (uint8_t)length)) {
        return;
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
            printk("PASS: %u idle keepalive gaps, each within %u ms\n",
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
    uint8_t bytes[WIRE_FRAME_MAX_ENCODED];
    uint32_t tx_length = uart_emul_get_tx_data(WIRE_UART, bytes, sizeof(bytes));
    wire_frame_parser_ingest(&tx_parser, bytes, tx_length, on_tx_frame, NULL);
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
