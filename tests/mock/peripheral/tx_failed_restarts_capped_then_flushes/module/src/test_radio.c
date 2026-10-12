// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral, keepalives acked, the key press lost.
 * Press fails again on every TX restart, writes queued behind it never complete.
 * Exits 0 once the press restarts TX ESB_LINK_TX_RESTARTS_MAX times, then flushes once.
 * Exits 1 on a wrong restart or flush count, or at deadline.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "esb_link_internal.h"
#include "mock.h"
#include "mock_esb.h"

#define FIRST_TRY_ATTEMPTS 1
#define VERDICT_DEADLINE_MS 1000

static bool press_sent;
static size_t starts_at_press;
static size_t flushes_at_press;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void tx_failed_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_failed();
}
static K_WORK_DEFINE(tx_failed_work, tx_failed_fn);

static bool link_searching(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    return status.searching;
}

static bool press_failing(void) {
    return press_sent && mock_esb_flush_count() == flushes_at_press;
}

static int restart_failed_head(void) {
    mock_check(press_failing(), "TX restarts only on the failed press before its flush");
    k_work_submit(&tx_failed_work);
    return 0;
}

static void check_press_outcome(void) {
    size_t restarts = mock_esb_tx_start_count() - starts_at_press;
    bool capped = restarts == ESB_LINK_TX_RESTARTS_MAX;
    mock_check(capped, "failed press restarts TX ESB_LINK_TX_RESTARTS_MAX times");
    size_t flushes = mock_esb_flush_count() - flushes_at_press;
    bool flushed_once = flushes == 1;
    mock_check(flushed_once, "press failing every restart flushes the TX FIFO once");
    printk("PASS: all %u checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

int esb_write_payload(const struct esb_payload *payload) {
    if (press_failing()) {
        return 0;
    }
    if (press_sent) {
        check_press_outcome();
    }
    if (esb_keepalive_matches(payload->data, payload->length)) {
        k_work_submit(&tx_success_work);
        return 0;
    }
    mock_check(!link_searching(), "link up before the press");
    starts_at_press = mock_esb_tx_start_count();
    flushes_at_press = mock_esb_flush_count();
    press_sent = true;
    k_work_submit(&tx_failed_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: no packet after the failed press, %u TX restarts, %u flushes\n",
           (unsigned int)mock_esb_tx_start_count(),
           (unsigned int)mock_esb_flush_count());
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    mock_esb_set_start_tx_handler(restart_failed_head);
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
