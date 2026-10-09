// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver with a TX FIFO under a real esb_link.c on a peripheral tapping two keys.
 * Second press fails its first transmission.
 * Its release follows before the next keepalive.
 * Exits 0 once the lost press goes on air again from the TX FIFO before its release is written,
 * and the release goes on air after it.
 * Exits 1 when the release is written first, when the press is written again,
 * when a keepalive lists the lost press, or at deadline.
 */
#include <errno.h>
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
#define TX_FIFO_DEPTH 8
#define VERDICT_DEADLINE_MS 1500

static struct esb_payload tx_fifo[TX_FIFO_DEPTH];
static size_t fifo_front;
static size_t fifo_count;
static size_t flushes_seen;
static bool head_fails;

static size_t presses_written;
static bool lost_press_written;
static uint32_t lost_position;
static bool press_lost;
static bool press_retried;

static void transmit_head(void);

static void sync_flush(void) {
    size_t flushes = mock_esb_flush_count();
    if (flushes != flushes_seen) {
        flushes_seen = flushes;
        fifo_count = 0;
    }
}

static void tx_done_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (head_fails) {
        mock_esb_tx_failed();
        return;
    }
    if (fifo_count != 0) {
        fifo_front = (fifo_front + 1) % TX_FIFO_DEPTH;
        fifo_count--;
    }
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
    sync_flush();
    if (fifo_count != 0 && esb_is_idle()) {
        transmit_head();
    }
}
static K_WORK_DEFINE(tx_done_work, tx_done_fn);

static bool decode_key_event(const struct esb_payload *payload,
                             struct zmk_split_transport_peripheral_event *event) {
    return esb_wire_decode_event(payload->data, payload->length, event) != 0 &&
           event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT;
}

static void check_keepalive_on_air(const uint8_t *keepalive) {
    if (!press_lost || press_retried) {
        return;
    }
    mock_check(!esb_keepalive_bitmap_get(esb_keepalive_bitmap(keepalive), lost_position),
               "tap falls between two keepalives");
}

static void note_key_on_air(const struct esb_payload *payload) {
    struct zmk_split_transport_peripheral_event event = {0};
    if (!decode_key_event(payload, &event) || !lost_press_written ||
        event.data.key_position_event.position != lost_position) {
        return;
    }
    if (event.data.key_position_event.pressed == 0) {
        printk("PASS: lost press of position %u went on air again from the TX FIFO before its "
               "release\n",
               (unsigned int)lost_position);
        exit(0);
    }
    if (!press_lost) {
        press_lost = true;
        head_fails = true;
        return;
    }
    press_retried = true;
}

static void transmit_head(void) {
    const struct esb_payload *head = &tx_fifo[fifo_front];
    mock_esb_tx_begin();
    head_fails = false;
    if (esb_keepalive_matches(head->data, head->length)) {
        check_keepalive_on_air(head->data);
    } else {
        note_key_on_air(head);
    }
    k_work_submit(&tx_done_work);
}

static void check_key_written(const struct esb_payload *payload) {
    struct zmk_split_transport_peripheral_event event = {0};
    if (!decode_key_event(payload, &event)) {
        return;
    }
    uint32_t position = event.data.key_position_event.position;
    if (event.data.key_position_event.pressed == 0) {
        mock_check(!lost_press_written || position != lost_position || press_retried,
                   "lost press goes on air again before its release is written");
        return;
    }
    mock_check(!lost_press_written || position != lost_position,
               "lost press is not written again with a new PID");
    presses_written++;
    if (presses_written == LOST_PRESS_NUMBER) {
        lost_press_written = true;
        lost_position = position;
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    sync_flush();
    if (fifo_count == TX_FIFO_DEPTH) {
        return -ENOMEM;
    }
    check_key_written(payload);
    tx_fifo[(fifo_front + fifo_count) % TX_FIFO_DEPTH] = *payload;
    fifo_count++;
    if (esb_is_idle()) {
        transmit_head();
    }
    return 0;
}

static int start_tx(void) {
    sync_flush();
    if (!esb_is_idle()) {
        return -EBUSY;
    }
    if (fifo_count == 0) {
        return -ENODATA;
    }
    transmit_head();
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (press_lost) {
        printk("FAIL: release of position %u never went on air\n", (unsigned int)lost_position);
    } else {
        printk("FAIL: %u presses written, press %u never lost\n", (unsigned int)presses_written,
               (unsigned int)LOST_PRESS_NUMBER);
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    mock_esb_set_start_tx_handler(start_tx);
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
