// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Channel-hopping engine layered on the ESB transport.
 * Hop logic is a no-op when hop-channels lists a single channel.
 * Peripheral keepalive tick runs regardless: it carries the peripheral state
 * snapshot the central reconciles against.
 * Central: owns the epoch, votes to hop off a degrading channel.
 * Peripheral: adopts the epoch from beacons, sweeps to re-find it on loss.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Schedule per-role hop work.
 * Safe before esb_link_init runs, work is statically initialized.
 * Central: hop-decision window.
 * Peripheral: keepalive tick. */
void hop_start(void);

void hop_stop(void);

/* Called per received payload in the radio ISR.
 * rssi is the ESB sample magnitude (dBm is its negative).
 * Returns true for control packets the caller must not queue.
 * Central: always false, keepalives queue up for position reconcile.
 * Peripheral: epoch beacon. */
bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi);

/* Central: no-op.
 * Peripheral: acked transmit succeeded after this many attempts (1 = first try). */
void hop_note_tx_success(uint8_t attempts);

/* Central: no-op.
 * Peripheral: transmit exhausted its retransmits this window. */
void hop_note_tx_failed(void);

/* Central: no-op.
 * Peripheral: real data went out, next keepalive uses the fast rate. */
void hop_note_data_sent(void);

/* Channel the radio should currently tune to. */
uint8_t hop_current_channel(void);

#if defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
int hop_stage_beacon(uint8_t pipe, uint8_t hid_modifiers, uint8_t hid_indicators);

bool hop_pipe_needs_rendezvous(uint8_t pipe);
#endif
