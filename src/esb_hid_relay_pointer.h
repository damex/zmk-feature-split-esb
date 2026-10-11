// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * HID relay pointer report: ZMK's mouse report, id, buttons, then four int16 running totals.
 * Button changes queue like key reports.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#define ESB_HID_RELAY_POINTER_LENGTH 10
#define ESB_HID_RELAY_POINTER_BUTTONS_OFFSET 1
#define ESB_HID_RELAY_POINTER_MOTION_OFFSET 2
#define ESB_HID_RELAY_POINTER_AXES 4

#if defined(CONFIG_DT_HAS_ZMK_INPUT_PROCESSOR_ESB_RELAY_POINTER_ENABLED)
/* Next take writes a report even without news. */
void esb_hid_relay_pointer_refresh(void);

/* Radio ISR. Writes buttons and running totals into out, returns its length.
 * Returns 0 without room, or when nothing changed since the last written report
 * and no refresh is due. */
size_t esb_hid_relay_pointer_take(uint8_t *out, size_t room, bool changes_queued);

/* Radio ISR. Totals moved since the last written report. */
bool esb_hid_relay_pointer_motion_pending(void);

/* Radio ISR. Writes running totals into a queued mouse report, releases its buttons while paused.
 * Returns false for any other report. */
bool esb_hid_relay_pointer_stamp(uint8_t *report, size_t length);

/* Radio ISR. The report reached the ACK FIFO, it becomes the last written one. */
void esb_hid_relay_pointer_sent(const uint8_t *report);

/* Sets buttons to ZMK's held ones, or none while paused. */
void esb_hid_relay_pointer_reset(void);
#else
static inline void esb_hid_relay_pointer_refresh(void) {
}

static inline size_t esb_hid_relay_pointer_take(uint8_t *out, size_t room, bool changes_queued) {
    ARG_UNUSED(out);
    ARG_UNUSED(room);
    ARG_UNUSED(changes_queued);
    return 0;
}

static inline bool esb_hid_relay_pointer_motion_pending(void) {
    return false;
}

static inline bool esb_hid_relay_pointer_stamp(uint8_t *report, size_t length) {
    ARG_UNUSED(report);
    ARG_UNUSED(length);
    return false;
}

static inline void esb_hid_relay_pointer_sent(const uint8_t *report) {
    ARG_UNUSED(report);
}

static inline void esb_hid_relay_pointer_reset(void) {
}
#endif
