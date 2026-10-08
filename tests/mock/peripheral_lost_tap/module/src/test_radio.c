// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral tapping two keys.
 * Second press exhausts its retransmits.
 * Its release follows before the next keepalive.
 * Exits 0 once the lost press is offered to the radio again before its release.
 * Exits 1 when the release goes first, when a keepalive lists the lost press, or at deadline.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/types.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "esb_wire.h"
#include "mock.h"
#include "mock_esb.h"

#define FIRST_TRY_ATTEMPTS 1
#define LOST_PRESS_NUMBER 2
#define VERDICT_DEADLINE_MS 1500

static size_t presses_offered;
static bool press_lost;
static uint32_t lost_position;

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

static void check_keepalive(const uint8_t *keepalive) {
    if (!press_lost) {
        return;
    }
    mock_check(!esb_keepalive_bitmap_get(esb_keepalive_bitmap(keepalive), lost_position),
               "tap falls between two keepalives");
}

static void write_release(uint32_t position) {
    mock_check(!press_lost || position != lost_position,
               "lost press is offered to the radio again before its release");
    k_work_submit(&tx_success_work);
}

static void write_press(uint32_t position) {
    if (press_lost && position == lost_position) {
        printk("PASS: lost press of position %u offered to the radio again before its release\n",
               (unsigned int)position);
        exit(0);
    }
    presses_offered++;
    if (presses_offered != LOST_PRESS_NUMBER) {
        k_work_submit(&tx_success_work);
        return;
    }
    press_lost = true;
    lost_position = position;
    k_work_submit(&tx_failed_work);
}

static void write_event(const struct esb_payload *payload) {
    struct zmk_split_transport_peripheral_event event = {0};
    if (esb_wire_decode_event(payload->data, payload->length, &event) == 0 ||
        event.type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        k_work_submit(&tx_success_work);
        return;
    }
    if (event.data.key_position_event.pressed != 0) {
        write_press(event.data.key_position_event.position);
    } else {
        write_release(event.data.key_position_event.position);
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        check_keepalive(payload->data);
        k_work_submit(&tx_success_work);
    } else {
        write_event(payload);
    }
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (press_lost) {
        printk("FAIL: release of position %u never offered\n", (unsigned int)lost_position);
    } else {
        printk("FAIL: %u presses offered, press %u never lost\n", (unsigned int)presses_offered,
               (unsigned int)LOST_PRESS_NUMBER);
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
