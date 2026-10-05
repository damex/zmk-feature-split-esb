// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Emulated wire UART tap shared by mock cases.
 * Needs a chosen zmk,esb-wire on a zephyr,uart-emul node.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*mock_frame_callback_t)(const uint8_t *payload, size_t length);

/* Returns raw bytes the module sent, whole frames go to callback, NULL skips them. */
size_t mock_wire_tx_drain(mock_frame_callback_t callback);

/* Exits 1 unless the whole frame enters the UART. */
void mock_wire_rx_inject(const uint8_t *payload, size_t length);
