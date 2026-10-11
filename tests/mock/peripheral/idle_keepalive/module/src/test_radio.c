// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on an idle peripheral whose keepalives are acked.
 * Exits 0 once keepalives with the link up hold the idle-keepalive-ms cadence.
 * Exits 1 on a longer gap or at deadline.
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

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "mock_esb.h"

#define FIRST_TRY_ATTEMPTS 1
#define GAP_SLACK_MS 4
#define GAPS_CHECKED 8
#define VERDICT_DEADLINE_MS 4000

static const uint32_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);

static bool link_up_seen;
static uint32_t last_keepalive_ms;
static size_t gaps_checked;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static bool link_searching(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    return status.searching;
}

static void check_keepalive(void) {
    uint32_t now_ms = k_uptime_get_32();
    if (!link_up_seen) {
        if (link_searching()) {
            return;
        }
        link_up_seen = true;
        last_keepalive_ms = now_ms;
        return;
    }
    uint32_t gap_ms = now_ms - last_keepalive_ms;
    last_keepalive_ms = now_ms;
    if (gap_ms > idle_keepalive_ms + GAP_SLACK_MS) {
        printk("FAIL: idle keepalive gap %u ms after %u good gaps, idle-keepalive-ms is %u\n",
               (unsigned int)gap_ms, (unsigned int)gaps_checked, (unsigned int)idle_keepalive_ms);
        exit(1);
    }
    gaps_checked++;
    if (gaps_checked == GAPS_CHECKED) {
        printk("PASS: %u idle keepalive gaps with the link up, each within %u ms\n",
               (unsigned int)GAPS_CHECKED, (unsigned int)idle_keepalive_ms);
        exit(0);
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        check_keepalive();
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (link_up_seen) {
        printk("FAIL: %u of %u idle keepalive gaps by deadline\n", (unsigned int)gaps_checked,
               (unsigned int)GAPS_CHECKED);
    } else {
        printk("FAIL: link never came up\n");
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
