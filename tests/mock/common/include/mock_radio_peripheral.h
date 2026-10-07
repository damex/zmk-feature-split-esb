// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core under a real esb_link_peripheral.c.
 * Mock case links mock_esb.c and mock_hfclk.c for the radio and clock below.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Hands a received payload to the transport, exits 1 before esb_link_init. */
void mock_radio_peripheral_rx_deliver(uint8_t pipe, const uint8_t *data, size_t length);
