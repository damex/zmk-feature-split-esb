// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Modifiers and indicators packed into one word, so a peripheral thread reads a
 * coherent pair against a beacon writing it.
 */
#pragma once

#include <stdint.h>

#define ESB_HID_STATE_INDICATORS_SHIFT 8

static inline uint16_t esb_hid_state_pack(uint8_t modifiers, uint8_t indicators) {
    return (uint16_t)(modifiers | (indicators << ESB_HID_STATE_INDICATORS_SHIFT));
}

static inline uint8_t esb_hid_state_modifiers(uint16_t state) {
    return (uint8_t)state;
}

static inline uint8_t esb_hid_state_indicators(uint16_t state) {
    return (uint8_t)(state >> ESB_HID_STATE_INDICATORS_SHIFT);
}
