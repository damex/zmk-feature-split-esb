// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral sending lossy and acked data.
 * Exits 0 once a window's keepalive serves as its ACK probe, acked data skips the next
 * keepalive, and the first lossy send of that window requests the ACK alone.
 * Exits 1 on a wrong ACK request, an extra keepalive, or at deadline.
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
#include "esb_link.h"
#include "mock.h"
#include "mock_esb.h"

#define WINDOW_MS DT_INST_PROP(0, hop_window_ms)
#define HALF_WINDOW_MS (WINDOW_MS / 2)
#define FIRST_TRY_ATTEMPTS 1
#define DATA_MARKER 0xA5
#define VERDICT_DEADLINE_MS 2000

BUILD_ASSERT(DT_INST_PROP(0, hop_window_ms) == DT_INST_PROP(0, idle_keepalive_ms),
             "expected windows need one tick period, active or idle");

static bool script_started;
static size_t keepalives;
static size_t keepalives_after_acked_data;
static bool last_data_noack;

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

static void send_data(bool ack) {
    const uint8_t data[] = {DATA_MARKER, DATA_MARKER};
    mock_check(esb_link_send(data, sizeof(data), ack) == 0, "data send accepted");
}

static void window_without_keepalive_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(keepalives == keepalives_after_acked_data,
               "window after acked data sends no keepalive");
    send_data(false);
    mock_check(!last_data_noack, "first lossy send of a window without keepalive requests the ACK");
    send_data(false);
    mock_check(last_data_noack, "second lossy send in that window skips the ACK");
    printk("PASS: all %u ack probe checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(window_without_keepalive_work, window_without_keepalive_fn);

static void window_with_keepalive_fn(struct k_work *work) {
    ARG_UNUSED(work);
    send_data(false);
    mock_check(last_data_noack, "lossy send after the window's keepalive skips the ACK");
    send_data(true);
    mock_check(!last_data_noack, "acked send requests the ACK");
    keepalives_after_acked_data = keepalives;
    k_work_reschedule(&window_without_keepalive_work, K_MSEC(WINDOW_MS));
}
static K_WORK_DELAYABLE_DEFINE(window_with_keepalive_work, window_with_keepalive_fn);

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        keepalives++;
        if (!script_started && !link_searching()) {
            script_started = true;
            k_work_reschedule(&window_with_keepalive_work, K_MSEC(HALF_WINDOW_MS));
        }
    } else {
        last_data_noack = payload->noack != 0;
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (script_started) {
        printk("FAIL: ack probe script stopped before its verdict\n");
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
