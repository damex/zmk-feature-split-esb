// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Relay dongle counters, logged once a second at debug level. */
#pragma once

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

enum esb_hid_relay_stat {
    ESB_HID_RELAY_STAT_POLL_SENT,
    ESB_HID_RELAY_STAT_POLL_REFUSED,
    ESB_HID_RELAY_STAT_TX_ACKED,
    ESB_HID_RELAY_STAT_TX_FAILED,
    ESB_HID_RELAY_STAT_NEW_MOTION,
    ESB_HID_RELAY_STAT_USB_TOOK,
    ESB_HID_RELAY_STAT_USB_REFUSED,
    ESB_HID_RELAY_STAT_COUNT,
};

#if defined(CONFIG_ZMK_SPLIT_ESB_HID_RELAY) && !defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&         \
    defined(CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL) && CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL >= LOG_LEVEL_DBG
/* Any context, radio ISR included. */
void esb_hid_relay_stat_note(enum esb_hid_relay_stat stat);
#else
static inline void esb_hid_relay_stat_note(enum esb_hid_relay_stat stat) {
    ARG_UNUSED(stat);
}
#endif
