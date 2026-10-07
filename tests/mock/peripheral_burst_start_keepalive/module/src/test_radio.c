// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral holding a key through an idle tick.
 * Release right after that tick, the first send of a burst, is lost after its ACK.
 * Exits 0 once a keepalive releases the key within two hop windows.
 * Exits 1 on a later keepalive or none at all.
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

#include <zmk/events/position_state_changed.h>
#include <zmk/split/transport/types.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "esb_wire.h"
#include "mock.h"
#include "mock_esb.h"

#define FIRST_TRY_ATTEMPTS 1
#define HELD_POSITION 0
#define PRESS_DELAY_MS 10
#define RELEASE_DELAY_MS 4
#define QUIET_KEEPALIVE_AFTER_PRESS 2
#define HOP_WINDOW_MS DT_INST_PROP(0, hop_window_ms)
#define IDLE_KEEPALIVE_MS DT_INST_PROP(0, idle_keepalive_ms)
#define HEAL_BOUND_MS (2 * HOP_WINDOW_MS)
#define HEAL_DEADLINE_MS (2 * IDLE_KEEPALIVE_MS)
#define VERDICT_DEADLINE_MS 3000

BUILD_ASSERT(HEAL_BOUND_MS < IDLE_KEEPALIVE_MS, "bound tells a pulled-in tick from the idle one");

static bool press_scheduled;
static bool pressed;
static size_t keepalives_after_press;
static bool release_lost;
static uint32_t lost_ms;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void raise_position(bool state) {
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .position = HELD_POSITION,
        .state = state,
        .timestamp = k_uptime_get(),
    });
}

static void press_fn(struct k_work *work) {
    ARG_UNUSED(work);
    pressed = true;
    raise_position(true);
}
static K_WORK_DELAYABLE_DEFINE(press_work, press_fn);

static void release_fn(struct k_work *work) {
    ARG_UNUSED(work);
    raise_position(false);
}
static K_WORK_DELAYABLE_DEFINE(release_work, release_fn);

static void heal_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: no keepalive released position %u within %u ms of its lost release\n",
           (unsigned int)HELD_POSITION, (unsigned int)HEAL_DEADLINE_MS);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(heal_deadline_work, heal_deadline_fn);

static bool link_searching(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    return status.searching;
}

static void check_heal(const uint8_t *keepalive) {
    if (esb_keepalive_bitmap_get(esb_keepalive_bitmap(keepalive), HELD_POSITION)) {
        return;
    }
    uint32_t heal_ms = k_uptime_get_32() - lost_ms;
    if (heal_ms > HEAL_BOUND_MS) {
        printk("keepalive released position %u %u ms after its lost release\n",
               (unsigned int)HELD_POSITION, (unsigned int)heal_ms);
    }
    mock_check(heal_ms <= HEAL_BOUND_MS,
               "keepalive heals a release lost at burst start within two hop windows");
    printk("PASS: keepalive released position %u %u ms after its lost release at burst start\n",
           (unsigned int)HELD_POSITION, (unsigned int)heal_ms);
    exit(0);
}

static void check_keepalive(const uint8_t *keepalive) {
    if (release_lost) {
        check_heal(keepalive);
        return;
    }
    if (link_searching()) {
        return;
    }
    if (!press_scheduled) {
        press_scheduled = true;
        k_work_reschedule(&press_work, K_MSEC(PRESS_DELAY_MS));
        return;
    }
    if (!pressed) {
        return;
    }
    keepalives_after_press++;
    if (keepalives_after_press == QUIET_KEEPALIVE_AFTER_PRESS) {
        k_work_reschedule(&release_work, K_MSEC(RELEASE_DELAY_MS));
    }
}

static void note_release(const struct esb_payload *payload) {
    struct zmk_split_transport_peripheral_event event;
    if (esb_wire_decode_event(payload->data, payload->length, &event) == 0) {
        return;
    }
    if (event.type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        return;
    }
    if (event.data.key_position_event.pressed) {
        return;
    }
    release_lost = true;
    lost_ms = k_uptime_get_32();
    k_work_reschedule(&heal_deadline_work, K_MSEC(HEAL_DEADLINE_MS));
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        check_keepalive(payload->data);
    } else {
        note_release(payload);
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: release never went out, pressed %d\n", pressed);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
