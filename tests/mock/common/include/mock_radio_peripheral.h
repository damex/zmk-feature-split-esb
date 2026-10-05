// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB radio and ESB link core under a real esb_link_peripheral.c.
 * Mock case defines esb_write_payload to observe transmits.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Hands a received payload to the transport, exits 1 before esb_link_init. */
void mock_radio_peripheral_rx_deliver(uint8_t pipe, const uint8_t *data, size_t length);

/* Last channel passed to esb_set_rf_channel, 0 before the first. */
uint32_t mock_radio_peripheral_channel(void);

size_t mock_radio_peripheral_channel_set_count(void);
