// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * HID relay pointer report: ZMK's mouse report, id, buttons, then four int16 deltas.
 * Central sums motion between relay polls and drains it into the relay ACK.
 * Button changes queue like key reports.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#define ESB_HID_RELAY_POINTER_LENGTH 10
#define ESB_HID_RELAY_POINTER_BUTTONS_OFFSET 1
#define ESB_HID_RELAY_POINTER_MOTION_OFFSET 2

#if defined(CONFIG_DT_HAS_ZMK_INPUT_PROCESSOR_ESB_RELAY_POINTER_ENABLED)
/* Radio ISR. Writes current buttons and pending motion into out, returns its length.
 * Returns 0 without room. */
size_t esb_hid_relay_pointer_take(uint8_t *out, size_t room);

/* Radio ISR. The report from take reached the ACK FIFO, its deltas leave the sums. */
void esb_hid_relay_pointer_sent(const uint8_t *report);

/* Clears pending motion and sets buttons to ZMK's held ones, or none while paused. */
void esb_hid_relay_pointer_reset(void);
#else
static inline size_t esb_hid_relay_pointer_take(uint8_t *out, size_t room) {
    ARG_UNUSED(out);
    ARG_UNUSED(room);
    return 0;
}

static inline void esb_hid_relay_pointer_sent(const uint8_t *report) {
    ARG_UNUSED(report);
}

static inline void esb_hid_relay_pointer_reset(void) {
}
#endif
