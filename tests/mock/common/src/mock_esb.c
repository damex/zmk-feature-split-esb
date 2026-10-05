// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Fake NCS ESB driver for both roles. */

#include "mock_esb.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

BUILD_ASSERT(CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "set CONFIG_ESB_MAX_PAYLOAD_LENGTH >= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD in the case .conf");

/* Room to overflow the module's RX ring in one event. */
#define RX_FIFO_DEPTH (2 * CONFIG_ZMK_SPLIT_ESB_RX_QUEUE_SIZE)

static esb_event_handler event_handler;
static struct esb_payload rx_fifo[RX_FIFO_DEPTH];
static size_t rx_fifo_head;
static size_t rx_fifo_count;
static uint32_t rf_channel;
static size_t rf_channel_sets;
static size_t tx_flushes;

int esb_init(const struct esb_config *config) {
    event_handler = config->event_handler;
    return 0;
}

bool esb_is_idle(void) {
    return true;
}

int esb_read_rx_payload(struct esb_payload *payload) {
    if (rx_fifo_count == 0) {
        return -ENODATA;
    }
    *payload = rx_fifo[rx_fifo_head];
    rx_fifo_head = (rx_fifo_head + 1) % RX_FIFO_DEPTH;
    rx_fifo_count--;
    return 0;
}

int esb_start_rx(void) {
    return 0;
}

int esb_stop_rx(void) {
    return 0;
}

int esb_flush_tx(void) {
    tx_flushes++;
    return 0;
}

int esb_set_address_length(uint8_t length) {
    ARG_UNUSED(length);
    return 0;
}

int esb_set_base_address_0(const uint8_t *addr) {
    ARG_UNUSED(addr);
    return 0;
}

int esb_set_base_address_1(const uint8_t *addr) {
    ARG_UNUSED(addr);
    return 0;
}

int esb_set_prefixes(const uint8_t *prefixes, uint8_t num_pipes) {
    ARG_UNUSED(prefixes);
    ARG_UNUSED(num_pipes);
    return 0;
}

int esb_set_rf_channel(uint32_t channel) {
    rf_channel = channel;
    rf_channel_sets++;
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

static void raise_event(struct esb_evt event) {
    if (event_handler == NULL) {
        printk("FAIL: ESB event before esb_init\n");
        exit(1);
    }
    event_handler(&event);
}

void mock_esb_rx_push(const struct esb_payload *payload) {
    if (rx_fifo_count == RX_FIFO_DEPTH) {
        printk("FAIL: fake ESB RX FIFO full\n");
        exit(1);
    }
    rx_fifo[(rx_fifo_head + rx_fifo_count) % RX_FIFO_DEPTH] = *payload;
    rx_fifo_count++;
}

void mock_esb_rx_raise(void) {
    raise_event((struct esb_evt){.evt_id = ESB_EVENT_RX_RECEIVED});
}

void mock_esb_tx_success(uint32_t attempts) {
    raise_event((struct esb_evt){.evt_id = ESB_EVENT_TX_SUCCESS, .tx_attempts = attempts});
}

void mock_esb_tx_failed(void) {
    raise_event((struct esb_evt){.evt_id = ESB_EVENT_TX_FAILED});
}

uint32_t mock_esb_channel(void) {
    return rf_channel;
}

size_t mock_esb_channel_set_count(void) {
    return rf_channel_sets;
}

size_t mock_esb_flush_count(void) {
    return tx_flushes;
}
