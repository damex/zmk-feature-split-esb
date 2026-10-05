// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral, keepalives acked, the key press lost.
 * Exits 0 once the failed press flushes the TX FIFO once, drops the link to searching,
 * and the next keepalive still carries the pressed key.
 * Exits 1 on a wrong flush count, link state or bitmap, or at deadline.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "mock.h"
#include "mock_esb.h"

#define PRESSED_POSITION 0
#define FIRST_TRY_ATTEMPTS 1
#define VERDICT_DEADLINE_MS 1000

static bool press_sent;
static bool failure_raised;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void tx_failed_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_failed();
    failure_raised = true;
}
static K_WORK_DEFINE(tx_failed_work, tx_failed_fn);

static bool link_searching(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    return status.searching;
}

static void check_press(void) {
    mock_check(!link_searching(), "link up before the press");
    mock_check(mock_esb_flush_count() == 0, "no TX flush before the press fails");
}

static void check_after_failure(const uint8_t *keepalive) {
    mock_check(mock_esb_flush_count() == 1, "exhausted retransmits flush the TX FIFO once");
    mock_check(link_searching(), "failed transmit drops the link to searching");
    mock_check(esb_keepalive_bitmap_get(esb_keepalive_bitmap(keepalive), PRESSED_POSITION),
               "keepalive after the failure still carries the pressed key");
    printk("PASS: all %u tx failure checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        if (failure_raised) {
            check_after_failure(payload->data);
        }
        k_work_submit(&tx_success_work);
        return 0;
    }
    if (!press_sent) {
        press_sent = true;
        check_press();
        k_work_submit(&tx_failed_work);
        return 0;
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (press_sent) {
        printk("FAIL: no keepalive after the failed press\n");
    } else {
        printk("FAIL: the key press never reached the radio\n");
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
