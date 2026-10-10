// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * HID state word, modifiers and indicators.
 * One word, so a reader never pairs modifiers and indicators from two beacons.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#define ESB_HID_STATE_INDICATORS_SHIFT 8

/* Relay dongle to central: host LED state. */
#define ESB_HOST_INDICATORS_TAG 0xFC
struct esb_host_indicators {
    uint8_t tag;
    uint8_t indicators;
} __attribute__((packed));
#define ESB_HOST_INDICATORS_LENGTH 2
BUILD_ASSERT(sizeof(struct esb_host_indicators) == ESB_HOST_INDICATORS_LENGTH,
             "host indicators wire size");

static inline bool esb_is_host_indicators(const uint8_t *data, size_t length) {
    if (length != ESB_HOST_INDICATORS_LENGTH) {
        return false;
    }
    return data[offsetof(struct esb_host_indicators, tag)] == ESB_HOST_INDICATORS_TAG;
}

/* Relay dongle to central: asks for the ACK alone, NCS refuses an empty payload. */
#define ESB_RELAY_POLL_TAG 0xFB
struct esb_relay_poll {
    uint8_t tag;
} __attribute__((packed));
#define ESB_RELAY_POLL_LENGTH 1
BUILD_ASSERT(sizeof(struct esb_relay_poll) == ESB_RELAY_POLL_LENGTH, "relay poll wire size");

static inline bool esb_is_relay_poll(const uint8_t *data, size_t length) {
    if (length != ESB_RELAY_POLL_LENGTH) {
        return false;
    }
    return data[offsetof(struct esb_relay_poll, tag)] == ESB_RELAY_POLL_TAG;
}

static inline uint16_t esb_hid_state_pack(uint8_t modifiers, uint8_t indicators) {
    return (uint16_t)(modifiers | (indicators << ESB_HID_STATE_INDICATORS_SHIFT));
}

static inline uint8_t esb_hid_state_modifiers(uint16_t state) {
    return (uint8_t)state;
}

static inline uint8_t esb_hid_state_indicators(uint16_t state) {
    return (uint8_t)(state >> ESB_HID_STATE_INDICATORS_SHIFT);
}
