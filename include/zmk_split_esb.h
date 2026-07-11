// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Read-only ESB link status, for display widgets and diagnostics.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct zmk_split_esb_status {
    uint8_t channel;
    uint8_t epoch;
    bool searching;
    int8_t rssi_dbm;
};

/* Lock-free snapshot of local link state.
 * rssi_dbm is received signal, never own transmit.
 * Central: worst sampled peripheral link.
 * Peripheral: own central link. */
void zmk_split_esb_get_status(struct zmk_split_esb_status *status);

/* Links tracked.
 * Central: served peripherals.
 * Peripheral: 1. */
uint8_t zmk_split_esb_pipe_count(void);

/* Received signal for one link, 0 before first sample or out of range.
 * Central: indexed by ESB pipe.
 * Peripheral: index 0 only. */
int8_t zmk_split_esb_pipe_rssi_dbm(uint8_t pipe);

/* Battery of any peripheral by ESB pipe, 0xFF when unknown.
 * Central: tracked from keepalives.
 * Peripheral: relayed by the central beacon. */
uint8_t zmk_split_esb_peer_battery(uint8_t pipe);

/* Central-measured RSSI of any peripheral by ESB pipe, 0 before first sample.
 * Central: own measurement.
 * Peripheral: relayed by the central beacon. */
int8_t zmk_split_esb_peer_rssi_dbm(uint8_t pipe);

/* Central: own HID state.
 * Peripheral: last byte synced from central. */
uint8_t zmk_split_esb_hid_modifiers(void);

/* Host LED bits, zmk_hid_indicators_t layout, 0 when CONFIG_ZMK_HID_INDICATORS off.
 * Central: own state.
 * Peripheral: last byte synced from central. */
uint8_t zmk_split_esb_hid_indicators(void);
