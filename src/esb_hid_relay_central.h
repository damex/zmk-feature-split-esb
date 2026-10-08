// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay, central half. */
#pragma once

#include <stdbool.h>

#if defined(CONFIG_ZMK_SPLIT_ESB_HID_RELAY)
/* False while the central has a host of its own, the relay dongle stays released then. */
bool esb_hid_relay_active(void);
#else
static inline bool esb_hid_relay_active(void) {
    return false;
}
#endif
