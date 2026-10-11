// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral, keepalives acked, the key press lost.
 * Boot keepalive fails while the link still searches.
 * First keepalive with the link up fails once and its TX restart succeeds.
 * Press fails again on every TX restart.
 * Writes queued behind the failing press never complete.
 * Exits 0 once the searching failure flushes without a TX restart,
 * the failed press restarts TX ESB_LINK_TX_RESTARTS_MAX times without a flush,
 * then flushes the TX FIFO once, drops the link to searching,
 * and the next keepalive still carries the pressed key.
 * Exits 1 on a wrong restart or flush count, link state or bitmap, or at deadline.
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
#include "esb_link_internal.h"
#include "mock.h"
#include "mock_esb.h"

#define PRESSED_POSITION 0
#define FIRST_TRY_ATTEMPTS 1
#define VERDICT_DEADLINE_MS 1000

static bool boot_keepalive_failed;
static bool keepalive_failed;
static bool press_sent;
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
    if (!press_sent) {
        k_work_submit(&tx_success_work);
        return 0;
    }
    mock_check(press_failing(), "TX restarts on the failed press before its flush");
    k_work_submit(&tx_failed_work);
    return 0;
}

static void check_press(void) {
    mock_check(!link_searching(), "link up before the press");
    mock_check(keepalive_failed, "keepalive with the link up failed before the press");
}

static void check_after_failure(const uint8_t *keepalive) {
    mock_check(mock_esb_tx_start_count() == 1 + ESB_LINK_TX_RESTARTS_MAX,
               "failed press restarts TX ESB_LINK_TX_RESTARTS_MAX times after a recovered keepalive");
    mock_check(mock_esb_flush_count() == flushes_at_press + 1,
               "press failing every restart flushes the TX FIFO once");
    mock_check(link_searching(), "failed transmit drops the link to searching");
    mock_check(esb_keepalive_bitmap_get(esb_keepalive_bitmap(keepalive), PRESSED_POSITION),
               "keepalive after the failure still carries the pressed key");
    printk("PASS: all %u tx failure checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

static void write_keepalive(const uint8_t *keepalive) {
    if (!boot_keepalive_failed) {
        mock_check(link_searching(), "link searches at boot");
        boot_keepalive_failed = true;
        k_work_submit(&tx_failed_work);
        return;
    }
    if (press_sent) {
        check_after_failure(keepalive);
    }
    if (!keepalive_failed) {
        mock_check(mock_esb_tx_start_count() == 0 && mock_esb_flush_count() == 1,
                   "failure while searching flushes without a TX restart");
    }
    if (!keepalive_failed && !link_searching()) {
        keepalive_failed = true;
        k_work_submit(&tx_failed_work);
        return;
    }
    k_work_submit(&tx_success_work);
}

int esb_write_payload(const struct esb_payload *payload) {
    if (press_failing()) {
        return 0;
    }
    if (esb_keepalive_matches(payload->data, payload->length)) {
        write_keepalive(payload->data);
        return 0;
    }
    if (!press_sent) {
        check_press();
        flushes_at_press = mock_esb_flush_count();
        press_sent = true;
        k_work_submit(&tx_failed_work);
        return 0;
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (press_sent) {
        printk("FAIL: no keepalive after the failed press, %u TX restarts, %u flushes\n",
               (unsigned int)mock_esb_tx_start_count(), (unsigned int)mock_esb_flush_count());
    } else {
        printk("FAIL: the key press never reached the radio\n");
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    mock_esb_set_start_tx_handler(restart_failed_head);
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
