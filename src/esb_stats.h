// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Link counters for every role, logged and cleared once a second at debug level. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

enum esb_stat {
    ESB_STAT_SENT,
    ESB_STAT_SEND_REFUSED,
    ESB_STAT_NOACK_SENT,
    ESB_STAT_TX_OK,
    ESB_STAT_TX_RETRIES,
    ESB_STAT_TX_FAILED,
    ESB_STAT_TX_FLUSHED,
    ESB_STAT_RELAY_POLLS,
    ESB_STAT_RELAY_NEW_MOTION,
    ESB_STAT_RELAY_USB_TOOK,
    ESB_STAT_RELAY_USB_REFUSED,
    ESB_STAT_REPLY_WRITTEN,
    ESB_STAT_REPLY_REFUSED,
    ESB_STAT_COUNT,
};

#if (defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ||                                                     \
     defined(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_TRANSPORT_ESB)) &&                                    \
    defined(CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL) && CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL >= LOG_LEVEL_DBG
/* Any context, radio ISR included. */
void esb_stat_add(enum esb_stat stat, uint32_t amount);

/* Central radio ISR. Data: neither keepalive nor relay poll. */
void esb_stat_note_rx(uint8_t pipe, bool data);
#else
static inline void esb_stat_add(enum esb_stat stat, uint32_t amount) {
    ARG_UNUSED(stat);
    ARG_UNUSED(amount);
}

static inline void esb_stat_note_rx(uint8_t pipe, bool data) {
    ARG_UNUSED(pipe);
    ARG_UNUSED(data);
}
#endif
