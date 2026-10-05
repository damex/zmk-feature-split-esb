// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for the ESB link on a simplex wire relay half.
 * Exits 0 once uplink relays and own commands run while nothing leaves on the wire.
 * Exits 1 on a wire transmit, a wire peer command run locally or a missed uplink.
 */
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

#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/split/transport/types.h>

#include "esb_link.h"
#include "hop.h"
#include "mock_wire.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define PEER_PIPE DT_PROP(DT_CHOSEN(zmk_esb_wire_peer), pipe)
#define DELIVER_DELAY_MS 50
#define TX_POLL_MS 4
#define WIRE_SILENCE_MS (4 * CONFIG_ZMK_SPLIT_ESB_WIRE_KEEPALIVE_MS)
#define OWN_INDICATORS 0x01
#define PEER_INDICATORS 0x02

static const struct zmk_split_transport_central_command own_command = {
    .type = ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS,
    .data.set_hid_indicators.indicators = OWN_INDICATORS,
};

static const struct zmk_split_transport_central_command peer_command = {
    .type = ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS,
    .data.set_hid_indicators.indicators = PEER_INDICATORS,
};

static const uint8_t uplink_payload[] = {0xB1, 0xB2, 0xB3};

static esb_link_rx_callback_t rx_callback;
static bool own_command_ran;
static bool uplink_relayed;

int esb_link_init(esb_link_rx_callback_t callback) {
    rx_callback = callback;
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    return 0;
}

int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    ARG_UNUSED(data);
    ARG_UNUSED(length);
    ARG_UNUSED(ack);
    return 0;
}

int esb_link_send_relay(const uint8_t *data, size_t length, bool ack) {
    ARG_UNUSED(ack);
    if (length == sizeof(uplink_payload) && memcmp(data, uplink_payload, length) == 0) {
        uplink_relayed = true;
    }
    return 0;
}

void hop_restore(void) {
}

uint8_t hop_link_cost_x10(void) {
    return 0;
}

static int indicators_listener(const zmk_event_t *event) {
    const struct zmk_hid_indicators_changed *changed = as_zmk_hid_indicators_changed(event);
    if (changed == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (changed->indicators == PEER_INDICATORS) {
        printk("FAIL: wire peer command ran on the relay half\n");
        exit(1);
    }
    if (changed->indicators == OWN_INDICATORS) {
        own_command_ran = true;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(wire_relay_simplex_test, indicators_listener);
ZMK_SUBSCRIPTION(wire_relay_simplex_test, zmk_hid_indicators_changed);

static void tx_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tx_poll_work, tx_poll_fn);

static void tx_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    size_t tx_length = mock_wire_tx_drain(NULL);
    if (tx_length > 0) {
        printk("FAIL: relay half transmitted %u bytes on a simplex wire\n",
               (unsigned int)tx_length);
        exit(1);
    }
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
}

static void deliver_fn(struct k_work *work) {
    ARG_UNUSED(work);
    rx_callback(PEER_PIPE, (const uint8_t *)&peer_command, sizeof(peer_command));
    rx_callback(SELF_PIPE, (const uint8_t *)&own_command, sizeof(own_command));
    mock_wire_rx_inject(uplink_payload, sizeof(uplink_payload));
}
static K_WORK_DELAYABLE_DEFINE(deliver_work, deliver_fn);

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (!uplink_relayed) {
        printk("FAIL: wire peer frame never relayed onto its ESB pipe\n");
        exit(1);
    }
    if (!own_command_ran) {
        printk("FAIL: relay half own command never ran\n");
        exit(1);
    }
    printk("PASS: uplink relayed and own command ran, nothing left on the simplex wire\n");
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&deliver_work, K_MSEC(DELIVER_DELAY_MS));
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
    k_work_reschedule(&verdict_work, K_MSEC(WIRE_SILENCE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
