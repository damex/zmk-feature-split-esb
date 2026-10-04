// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Test radio, NCS ESB subset the central link and hop engine use. */
#pragma once

#include <stdint.h>

enum esb_mode {
    ESB_MODE_PTX,
    ESB_MODE_PRX,
};

struct esb_payload {
    uint8_t length;
    uint8_t pipe;
    int8_t rssi;
    uint8_t noack;
    uint8_t pid;
    uint8_t data[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
};

int esb_write_payload(const struct esb_payload *payload);
int esb_start_rx(void);
int esb_stop_rx(void);
int esb_set_rf_channel(uint32_t channel);
