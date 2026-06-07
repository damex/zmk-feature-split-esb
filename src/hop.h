// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Channel-hopping engine layered on the ESB transport.
 * The central owns the epoch and votes to hop off a degrading channel.
 * A peripheral adopts the central's epoch from beacons and sweeps to re-find it on loss.
 * Every entry point is a no-op when hop-channels lists a single channel.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Schedule the per-role hop work.
 * Safe before esb_link_init runs, since the work is statically initialized. */
void hop_start(void);

void hop_stop(void);

/* Called per received payload in the radio ISR.
 * Returns true for a control packet the caller must not queue: a keepalive on the
 * central, an epoch beacon on a peripheral. */
bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length);

/* Peripheral: a transmit exhausted its retransmits this window. */
void hop_note_tx_failed(void);

/* Peripheral: real data went out, so the next keepalive uses the fast rate. */
void hop_note_data_sent(void);

/* Channel the radio should currently tune to. */
uint8_t hop_current_channel(void);
