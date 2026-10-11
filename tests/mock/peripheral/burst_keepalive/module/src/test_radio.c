// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral typing a steady burst.
 * First key release is lost after its ACK, typing keeps every hop window busy.
 * Exits 0 once a keepalive releases the lost key within idle-keepalive-ms plus one hop window.
 * Exits 1 on a gap in typing, typing stopping before the heal, or at that bound.
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

#include <zmk/split/transport/types.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "esb_wire.h"
#include "mock.h"
#include "mock_esb.h"

#define FIRST_TRY_ATTEMPTS 1
#define HOP_WINDOW_MS DT_INST_PROP(0, hop_window_ms)
#define HEAL_BOUND_MS (DT_INST_PROP(0, idle_keepalive_ms) + HOP_WINDOW_MS)
#define VERDICT_DEADLINE_MS 3000

static bool release_lost;
static uint32_t lost_position;
static uint32_t lost_ms;
static uint32_t last_key_ms;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void heal_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: no keepalive released position %u within %u ms of its lost release\n",
           (unsigned int)lost_position, (unsigned int)HEAL_BOUND_MS);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(heal_deadline_work, heal_deadline_fn);

static void check_keepalive(const uint8_t *keepalive) {
    if (!release_lost || esb_keepalive_bitmap_get(esb_keepalive_bitmap(keepalive), lost_position)) {
        return;
    }
    uint32_t now_ms = k_uptime_get_32();
    mock_check(now_ms - last_key_ms <= HOP_WINDOW_MS, "typing still flowing when the keepalive heals");
    printk("PASS: keepalive released position %u %u ms after its lost release, mid-burst\n",
           (unsigned int)lost_position, (unsigned int)(now_ms - lost_ms));
    exit(0);
}

static void note_key_event(const struct esb_payload *payload) {
    struct zmk_split_transport_peripheral_event event;
    if (esb_wire_decode_event(payload->data, payload->length, &event) == 0) {
        return;
    }
    if (event.type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        return;
    }
    uint32_t now_ms = k_uptime_get_32();
    if (release_lost) {
        mock_check(now_ms - last_key_ms <= HOP_WINDOW_MS,
                   "typing leaves no hop window without a key event");
    }
    last_key_ms = now_ms;
    if (release_lost || event.data.key_position_event.pressed) {
        return;
    }
    release_lost = true;
    lost_position = event.data.key_position_event.position;
    lost_ms = now_ms;
    k_work_reschedule(&heal_deadline_work, K_MSEC(HEAL_BOUND_MS));
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        check_keepalive(payload->data);
    } else {
        note_key_event(payload);
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: no key release on the wire\n");
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
