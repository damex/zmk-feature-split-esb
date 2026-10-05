// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Emulated wire UART tap shared by mock cases. */

#include "mock_wire.h"

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "wire_frame.h"

#define WIRE_UART DEVICE_DT_GET(DT_CHOSEN(zmk_esb_wire))

static struct wire_frame_parser tx_parser;
static mock_frame_callback_t tx_callback;

static void on_tx_frame(const uint8_t *payload, size_t length, void *user_data) {
    ARG_UNUSED(user_data);
    if (tx_callback != NULL) {
        tx_callback(payload, length);
    }
}

size_t mock_wire_tx_drain(mock_frame_callback_t callback) {
    uint8_t bytes[WIRE_FRAME_MAX_ENCODED];
    uint32_t tx_length = uart_emul_get_tx_data(WIRE_UART, bytes, sizeof(bytes));
    tx_callback = callback;
    wire_frame_parser_ingest(&tx_parser, bytes, tx_length, on_tx_frame, NULL);
    return tx_length;
}

void mock_wire_rx_inject(const uint8_t *payload, size_t length) {
    uint8_t frame[WIRE_FRAME_MAX_ENCODED];
    int frame_length = wire_frame_encode(payload, length, frame, sizeof(frame));
    if (frame_length < 0) {
        printk("FAIL: wire frame encode returned %d\n", frame_length);
        exit(1);
    }
    uint32_t accepted = uart_emul_put_rx_data(WIRE_UART, frame, (size_t)frame_length);
    if (accepted != (uint32_t)frame_length) {
        printk("FAIL: wire rx took %u of %d bytes\n", (unsigned int)accepted, frame_length);
        exit(1);
    }
}
