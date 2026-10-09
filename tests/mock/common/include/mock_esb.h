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

/* Radio reads busy from here until the next TX event. */
void mock_esb_tx_begin(void);

/* Events run in the calling thread in place of the radio ISR, exit 1 before esb_init. */
void mock_esb_rx_raise(void);
void mock_esb_tx_success(uint32_t attempts);
void mock_esb_tx_failed(void);

/* Last channel esb_set_rf_channel took, 0 before the first.
 * Busy radio refuses a channel with -EBUSY, as NCS does without fast channel switching. */
uint32_t mock_esb_channel(void);

/* Last count passed to esb_set_retransmit_count, 0 before the first. */
uint16_t mock_esb_retransmit_count(void);

size_t mock_esb_channel_set_count(void);
size_t mock_esb_flush_count(void);
size_t mock_esb_calls_before_init(void);
bool mock_esb_initialized(void);
size_t mock_esb_rx_start_count(void);
size_t mock_esb_rx_stop_count(void);
size_t mock_esb_tx_start_count(void);

/* Runs inside esb_start_tx and supplies its return.
 * Unset, esb_start_tx returns 0. */
void mock_esb_set_start_tx_handler(int (*handler)(void));
