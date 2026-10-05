// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a peripheral with a key, a split button and an encoder.
 * Exits 0 once the keepalive after the last event carries every field and the next reports idle.
 * Exits 1 on a wrong field or at deadline.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/sensors.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "esb_sensor_sync.h"
#include "hop.h"
#include "mock.h"

#define INPUT_REG DT_REG_ADDR(DT_NODELABEL(split_input))
#define HELD_POSITION 0
#define SENSOR_INDEX 0
#define ROTATION_TOTAL_DEG (DT_FOREACH_PROP_ELEM_SEP(DT_NODELABEL(mock_encoder), events, DT_PROP_BY_IDX, (+)))
#define FIRST_TRY_COST_X10 10
#define VERDICT_DEADLINE_MS 2500

static bool last_event_seen;

static bool rotation_complete(const uint8_t *keepalive, uint8_t length) {
    if (esb_keepalive_sensor_count(keepalive, length) <= SENSOR_INDEX) {
        return false;
    }
    return esb_keepalive_sensor_total_udeg(keepalive, SENSOR_INDEX) ==
           esb_sensor_udeg(ROTATION_TOTAL_DEG, 0);
}

static void check_fields(const uint8_t *keepalive, uint8_t length) {
    uint8_t bitmap[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    esb_keepalive_bitmap_set(bitmap, HELD_POSITION, true);
    if (memcmp(esb_keepalive_bitmap(keepalive), bitmap, sizeof(bitmap)) != 0) {
        printk("keepalive bitmap:");
        mock_print_bytes(esb_keepalive_bitmap(keepalive), sizeof(bitmap));
    }
    mock_check(memcmp(esb_keepalive_bitmap(keepalive), bitmap, sizeof(bitmap)) == 0,
               "bitmap holds only the pressed key");
    mock_check(esb_keepalive_battery_level(keepalive) == ESB_KEEPALIVE_BATTERY_UNKNOWN,
               "battery unknown without battery reporting");
    mock_check(esb_keepalive_link_cost_x10(keepalive) == FIRST_TRY_COST_X10,
               "link cost at the first-try baseline");
    mock_check(esb_keepalive_held_count(keepalive) == 1, "one held input key");
    struct esb_keepalive_held_key key = esb_keepalive_held_key_at(keepalive, 0);
    mock_check(key.reg == INPUT_REG, "held key carries the input-split reg");
    mock_check(key.code == INPUT_BTN_0, "held key carries the button code");
    mock_check(esb_keepalive_sensor_count(keepalive, length) == ZMK_KEYMAP_SENSORS_LEN,
               "one total per keymap sensor after the held key");
    mock_check(rotation_complete(keepalive, length), "total sums every encoder step");
}

static void check_keepalive(const uint8_t *keepalive, uint8_t length) {
    if (!last_event_seen) {
        if (!rotation_complete(keepalive, length)) {
            return;
        }
        check_fields(keepalive, length);
        mock_check(esb_keepalive_state(keepalive) == ESB_KEEPALIVE_ACTIVE,
                   "keepalive after the last event reports active");
        last_event_seen = true;
        return;
    }
    check_fields(keepalive, length);
    mock_check(esb_keepalive_state(keepalive) == ESB_KEEPALIVE_IDLE,
               "keepalive with nothing sent since reports idle");
    printk("PASS: all %u keepalive field checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        check_keepalive(payload->data, payload->length);
    }
    return 0;
}

bool esb_is_idle(void) {
    return true;
}

int esb_flush_tx(void) {
    return 0;
}

int esb_set_tx_power(int8_t tx_output_power) {
    ARG_UNUSED(tx_output_power);
    return 0;
}

int esb_set_retransmit_delay(uint16_t delay) {
    ARG_UNUSED(delay);
    return 0;
}

int esb_set_retransmit_count(uint16_t count) {
    ARG_UNUSED(count);
    return 0;
}

int esb_set_rf_channel(uint32_t channel) {
    ARG_UNUSED(channel);
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    ARG_UNUSED(callback);
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    return 0;
}

int esb_link_hfclk_acquire(void) {
    return 0;
}

void esb_link_hfclk_release(void) {
}

void esb_link_mark_tx_event(void) {
}

uint32_t esb_link_tx_last_event_ms(void) {
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (last_event_seen) {
        printk("FAIL: no keepalive after the active one\n");
    } else {
        printk("FAIL: no keepalive carried the full encoder total\n");
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
