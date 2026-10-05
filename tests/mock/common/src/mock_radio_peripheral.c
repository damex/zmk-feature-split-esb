// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Fake NCS ESB radio and ESB link core under a real esb_link_peripheral.c. */

#include "mock_radio_peripheral.h"

#include <stdbool.h>
#include <stdlib.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"

static esb_link_rx_callback_t rx_callback;

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

void mock_radio_peripheral_rx_deliver(uint8_t pipe, const uint8_t *data, size_t length) {
    if (rx_callback == NULL) {
        printk("FAIL: radio delivery before esb_link_init\n");
        exit(1);
    }
    rx_callback(pipe, data, length);
}
