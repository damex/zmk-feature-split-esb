// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Test radio, NCS ESB subset the module calls. */
#pragma once

#include <stdbool.h>
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
bool esb_is_idle(void);
int esb_flush_tx(void);
int esb_set_tx_power(int8_t tx_output_power);
int esb_set_retransmit_delay(uint16_t delay);
int esb_set_retransmit_count(uint16_t count);
