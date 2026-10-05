// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a wire relay half.
 * Exits 0 once a wire peer beacon reaches the wire and applies locally, own beacon staying off.
 * Exits 1 on a leaked own beacon or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/modifiers.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"
#include "hop_internal.h"
#include "mock_wire.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define PEER_PIPE DT_PROP(DT_CHOSEN(zmk_esb_wire_peer), pipe)
#define DELIVER_DELAY_MS 50
#define TX_POLL_MS 4
#define VERDICT_DEADLINE_MS 1000
#define INDICATORS 0x02

static const struct esb_beacon own_beacon = {
    .tag = ESB_BEACON_TAG,
    .hid_modifiers = MOD_LCTL,
    .hid_indicators = INDICATORS,
};

static const struct esb_beacon peer_beacon = {
    .tag = ESB_BEACON_TAG,
    .hid_modifiers = MOD_LSFT,
    .hid_indicators = INDICATORS,
};

static esb_link_rx_callback_t rx_callback;

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
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
    rx_callback = callback;
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

static void radio_receive(uint8_t pipe, const struct esb_beacon *beacon) {
    const uint8_t *data = (const uint8_t *)beacon;
    if (!hop_consume_rx(pipe, data, sizeof(*beacon), 0)) {
        rx_callback(pipe, data, sizeof(*beacon));
    }
}

static bool beacon_matches(const uint8_t *payload, size_t length, const struct esb_beacon *beacon) {
    if (length != sizeof(*beacon)) {
        return false;
    }
    return memcmp(payload, beacon, length) == 0;
}

static void check_verdict(void) {
    uint8_t modifiers = zmk_split_esb_hid_modifiers();
    if (modifiers != peer_beacon.hid_modifiers) {
        printk("FAIL: relay half holds modifiers 0x%02x, last beacon carried 0x%02x\n", modifiers,
               peer_beacon.hid_modifiers);
        exit(1);
    }
    printk("PASS: wire peer beacon reached the wire and applied locally, own beacon stayed off\n");
    exit(0);
}

static void on_tx_frame(const uint8_t *payload, size_t length) {
    if (beacon_matches(payload, length, &own_beacon)) {
        printk("FAIL: relay half own beacon left on the wire\n");
        exit(1);
    }
    if (beacon_matches(payload, length, &peer_beacon)) {
        check_verdict();
    }
}

static void tx_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tx_poll_work, tx_poll_fn);

static void tx_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    (void)mock_wire_tx_drain(on_tx_frame);
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
}

static void deliver_fn(struct k_work *work) {
    ARG_UNUSED(work);
    radio_receive(SELF_PIPE, &own_beacon);
    radio_receive(PEER_PIPE, &peer_beacon);
}
static K_WORK_DELAYABLE_DEFINE(deliver_work, deliver_fn);

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: wire peer beacon never reached the wire\n");
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&deliver_work, K_MSEC(DELIVER_DELAY_MS));
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
