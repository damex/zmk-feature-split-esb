// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap on a simplex wire peer's UART.
 * Exits 0 once the peer keeps sending, ignores received frames and reports connected.
 * Exits 1 otherwise.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/modifiers.h>

#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include <zmk_split_esb.h>

#include "esb_keepalive.h"
#include "hop_internal.h"
#include "mock_wire.h"

#define INJECT_DELAY_MS 50
#define TX_POLL_MS 4
#define KEEPALIVES_MIN 2
#define VERDICT_MS (5 * DT_INST_PROP(0, idle_keepalive_ms))

static size_t keepalives_seen;

static void on_tx_frame(const uint8_t *payload, size_t length) {
    if (esb_keepalive_matches(payload, (uint8_t)length)) {
        keepalives_seen++;
    }
}

static void tx_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tx_poll_work, tx_poll_fn);

static void tx_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    (void)mock_wire_tx_drain(on_tx_frame);
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
}

static void inject_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct esb_beacon beacon = {
        .tag = ESB_BEACON_TAG,
        .hid_modifiers = MOD_LSFT,
    };
    mock_wire_rx_inject((const uint8_t *)&beacon, sizeof(beacon));
}
static K_WORK_DELAYABLE_DEFINE(inject_work, inject_fn);

static enum zmk_split_transport_connections_status transport_connections(void) {
    STRUCT_SECTION_FOREACH(zmk_split_transport_peripheral, transport) {
        return transport->api->get_status().connections;
    }
    return ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (zmk_split_esb_hid_modifiers() != 0) {
        printk("FAIL: wire peer took modifiers 0x%02x over a simplex wire\n",
               zmk_split_esb_hid_modifiers());
        exit(1);
    }
    if (keepalives_seen < KEEPALIVES_MIN) {
        printk("FAIL: wire peer sent %u keepalives, expected at least %u\n",
               (unsigned int)keepalives_seen, (unsigned int)KEEPALIVES_MIN);
        exit(1);
    }
    enum zmk_split_transport_connections_status connections = transport_connections();
    if (connections != ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED) {
        printk("FAIL: wire peer reports connections %d on a simplex wire\n", (int)connections);
        exit(1);
    }
    printk("PASS: simplex wire peer sent %u keepalives, ignored rx, reports connected\n",
           (unsigned int)keepalives_seen);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_wire_init(void) {
    k_work_reschedule(&inject_work, K_MSEC(INJECT_DELAY_MS));
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_wire_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
