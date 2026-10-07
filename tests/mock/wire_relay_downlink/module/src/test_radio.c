// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link on a wire relay half.
 * Exits 0 once own-pipe commands run locally and wire peer commands leave on the wire.
 * Exits 1 on a misrouted command or at deadline.
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
#define VERDICT_DEADLINE_MS 1000
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

static esb_link_rx_callback_t rx_callback;
static bool own_command_ran;
static bool peer_command_forwarded;

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
    ARG_UNUSED(data);
    ARG_UNUSED(length);
    ARG_UNUSED(ack);
    return 0;
}

void hop_restore(void) {
}

uint8_t hop_link_cost_x10(void) {
    return 0;
}

static void check_verdict(void) {
    if (own_command_ran && peer_command_forwarded) {
        printk("PASS: own command ran locally, wire peer command left on the wire\n");
        exit(0);
    }
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
        check_verdict();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(wire_relay_downlink_test, indicators_listener);
ZMK_SUBSCRIPTION(wire_relay_downlink_test, zmk_hid_indicators_changed);

static bool frame_matches(const uint8_t *payload, size_t length,
                          const struct zmk_split_transport_central_command *command) {
    if (length != sizeof(*command)) {
        return false;
    }
    return memcmp(payload, command, length) == 0;
}

static void on_tx_frame(const uint8_t *payload, size_t length) {
    if (frame_matches(payload, length, &own_command)) {
        printk("FAIL: relay half own command left on the wire\n");
        exit(1);
    }
    if (frame_matches(payload, length, &peer_command)) {
        peer_command_forwarded = true;
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
    /* Peer first: commands run in arrival order, so a misrouted peer command fails before own one passes. */
    rx_callback(PEER_PIPE, (const uint8_t *)&peer_command, sizeof(peer_command));
    rx_callback(SELF_PIPE, (const uint8_t *)&own_command, sizeof(own_command));
}
static K_WORK_DELAYABLE_DEFINE(deliver_work, deliver_fn);

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: own command ran %d, wire peer command forwarded %d\n", own_command_ran,
           peer_command_forwarded);
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
