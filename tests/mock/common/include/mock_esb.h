// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB driver for both roles.
 * Mock case defines esb_write_payload to observe transmits.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esb.h>

/* Queues a payload for the next RX event, exits 1 when the fake FIFO is full. */
void mock_esb_rx_push(const struct esb_payload *payload);

/* Events run in the calling thread in place of the radio ISR, exit 1 before esb_init. */
void mock_esb_rx_raise(void);
void mock_esb_tx_success(uint32_t attempts);
void mock_esb_tx_failed(void);

/* Last channel passed to esb_set_rf_channel, 0 before the first. */
uint32_t mock_esb_channel(void);

size_t mock_esb_channel_set_count(void);
size_t mock_esb_flush_count(void);
size_t mock_esb_calls_before_init(void);
bool mock_esb_initialized(void);
size_t mock_esb_rx_start_count(void);
size_t mock_esb_rx_stop_count(void);
