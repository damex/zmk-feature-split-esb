// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay, central half. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/sys/util.h>

#if defined(CONFIG_ZMK_SPLIT_ESB_HID_RELAY)
/* False while the central has a host of its own, the relay dongle stays released then. */
bool esb_hid_relay_active(void);

/* Queues report on every relay pipe, behind every report queued before it. */
void esb_hid_relay_stage(const void *report, size_t length);
#else
static inline bool esb_hid_relay_active(void) {
    return false;
}

static inline void esb_hid_relay_stage(const void *report, size_t length) {
    ARG_UNUSED(report);
    ARG_UNUSED(length);
}
#endif
