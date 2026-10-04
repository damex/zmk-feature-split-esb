// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test radio standing in for NCS ESB on a central with a wire peer.
 * Exits 0 once wire beacons carry modifier changes in order, then refresh unchanged.
 * Exits 1 on a wrong change or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/modifiers.h>

#include <esb.h>

#include "central.h"
#include "esb_keepalive.h"
#include "esb_link_internal.h"
#include "esb_survey.h"
#include "hop.h"
#include "hop_internal.h"
#include "wire_frame.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define WIRE_UART DEVICE_DT_GET(DT_CHOSEN(zmk_esb_wire))
#define TX_POLL_MS 4
#define HEARTBEAT_MS 100
#define VERDICT_DEADLINE_MS 2000

const uint8_t esb_link_pipe_count = DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals));

static const uint8_t expected_modifiers[] = {
    MOD_LSFT,
    0,
};
static size_t next_change;
static uint8_t last_modifiers;
static struct wire_frame_parser tx_parser;

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

int esb_start_rx(void) {
    return 0;
}

int esb_stop_rx(void) {
    return 0;
}

int esb_set_rf_channel(uint32_t channel) {
    ARG_UNUSED(channel);
    return 0;
}

void esb_survey_run(const uint8_t *channels, size_t count, int8_t *energy_dbm) {
    ARG_UNUSED(channels);
    memset(energy_dbm, INT8_MIN, count);
}

uint8_t esb_central_battery_level(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return ESB_KEEPALIVE_BATTERY_UNKNOWN;
}

void central_ingest_packet(uint8_t pipe, const uint8_t *data, size_t length) {
    ARG_UNUSED(pipe);
    ARG_UNUSED(data);
    ARG_UNUSED(length);
}

static void on_tx_frame(const uint8_t *payload, size_t length, void *user_data) {
    ARG_UNUSED(user_data);
    if (!esb_is_beacon(payload, (uint8_t)length)) {
        return;
    }
    struct esb_beacon beacon;
    memcpy(&beacon, payload, sizeof(beacon));
    if (next_change == ARRAY_SIZE(expected_modifiers)) {
        if (beacon.hid_modifiers != last_modifiers) {
            printk("FAIL: refresh carried modifiers 0x%02x, expected 0x%02x\n",
                   beacon.hid_modifiers, last_modifiers);
            exit(1);
        }
        printk("PASS: wire beacons carried all %u modifier changes in order, then refreshed\n",
               (unsigned int)ARRAY_SIZE(expected_modifiers));
        exit(0);
    }
    if (beacon.hid_modifiers == last_modifiers) {
        return;
    }
    last_modifiers = beacon.hid_modifiers;
    if (beacon.hid_modifiers != expected_modifiers[next_change]) {
        printk("FAIL: step %u of %u, expected modifiers 0x%02x, got 0x%02x\n",
               (unsigned int)(next_change + 1), (unsigned int)ARRAY_SIZE(expected_modifiers),
               expected_modifiers[next_change], beacon.hid_modifiers);
        exit(1);
    }
    next_change++;
}

static void tx_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tx_poll_work, tx_poll_fn);

static void tx_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t bytes[WIRE_FRAME_MAX_ENCODED];
    uint32_t tx_length = uart_emul_get_tx_data(WIRE_UART, bytes, sizeof(bytes));
    wire_frame_parser_ingest(&tx_parser, bytes, tx_length, on_tx_frame, NULL);
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
}

static void heartbeat_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_fn);

static void heartbeat_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t frame[WIRE_FRAME_MAX_ENCODED];
    int frame_length = wire_frame_encode(NULL, 0, frame, sizeof(frame));
    if (frame_length < 0) {
        printk("FAIL: heartbeat frame encode returned %d\n", frame_length);
        exit(1);
    }
    (void)uart_emul_put_rx_data(WIRE_UART, frame, (size_t)frame_length);
    k_work_reschedule(&heartbeat_work, K_MSEC(HEARTBEAT_MS));
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (next_change == ARRAY_SIZE(expected_modifiers)) {
        printk("FAIL: no refresh beacon on the wire after the last change\n");
    } else {
        printk("FAIL: wire beacons stopped at step %u of %u\n", (unsigned int)(next_change + 1),
               (unsigned int)ARRAY_SIZE(expected_modifiers));
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    hop_start();
    k_work_reschedule(&tx_poll_work, K_MSEC(TX_POLL_MS));
    k_work_reschedule(&heartbeat_work, K_NO_WAIT);
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
