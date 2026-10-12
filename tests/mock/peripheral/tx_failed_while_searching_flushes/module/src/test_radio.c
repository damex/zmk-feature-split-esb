// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral, the first packet fails at boot.
 * Exits 0 once the failure while the link searches flushes the TX FIFO without a TX restart.
 * Exits 1 on a TX restart, a missing flush, or at deadline.
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

#include "mock.h"
#include "mock_esb.h"

#define VERDICT_DEADLINE_MS 1000

static bool first_packet_failed;
static size_t flushes_before_failure;

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

static void check_failure_outcome(void) {
    bool restarted = mock_esb_tx_start_count() != 0;
    mock_check(!restarted, "failure while searching restarts no TX");
    bool flushed = mock_esb_flush_count() == flushes_before_failure + 1;
    mock_check(flushed, "failure while searching flushes the TX FIFO");
    printk("PASS: all %u checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    if (first_packet_failed) {
        check_failure_outcome();
    }
    mock_check(link_searching(), "link searches at boot");
    flushes_before_failure = mock_esb_flush_count();
    first_packet_failed = true;
    k_work_submit(&tx_failed_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: no packet after the failed one\n");
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
