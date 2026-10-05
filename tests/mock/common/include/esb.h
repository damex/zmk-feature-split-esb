// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Test radio, NCS ESB subset the module calls, signatures as in NCS. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESB_DEFAULT_CONFIG                                                                         \
    {                                                                                              \
        .protocol = ESB_PROTOCOL_ESB_DPL,                                                          \
        .mode = ESB_MODE_PTX,                                                                      \
        .event_handler = 0,                                                                        \
        .bitrate = ESB_BITRATE_2MBPS,                                                              \
        .crc = ESB_CRC_16BIT,                                                                      \
        .tx_output_power = 0,                                                                      \
        .retransmit_delay = 600,                                                                   \
        .retransmit_count = 3,                                                                     \
        .tx_mode = ESB_TXMODE_AUTO,                                                                \
        .payload_length = 32,                                                                      \
        .selective_auto_ack = false,                                                               \
        .use_fast_ramp_up = false,                                                                 \
    }

enum esb_protocol {
    ESB_PROTOCOL_ESB,
    ESB_PROTOCOL_ESB_DPL,
};

enum esb_mode {
    ESB_MODE_PTX,
    ESB_MODE_PRX,
};

enum esb_bitrate {
    ESB_BITRATE_1MBPS,
    ESB_BITRATE_2MBPS,
};

enum esb_crc {
    ESB_CRC_16BIT,
    ESB_CRC_8BIT,
    ESB_CRC_OFF,
};

enum esb_tx_mode {
    ESB_TXMODE_AUTO,
    ESB_TXMODE_MANUAL,
    ESB_TXMODE_MANUAL_START,
};

enum esb_evt_id {
    ESB_EVENT_TX_SUCCESS,
    ESB_EVENT_TX_FAILED,
    ESB_EVENT_RX_RECEIVED,
};

struct esb_payload {
    uint8_t length;
    uint8_t pipe;
    int8_t rssi;
    uint8_t noack;
    uint8_t pid;
    uint8_t data[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
};

struct esb_evt {
    enum esb_evt_id evt_id;
    uint32_t tx_attempts;
};

typedef void (*esb_event_handler)(const struct esb_evt *event);

struct esb_config {
    enum esb_protocol protocol;
    enum esb_mode mode;
    esb_event_handler event_handler;
    enum esb_bitrate bitrate;
    enum esb_crc crc;
    int8_t tx_output_power;
    uint16_t retransmit_delay;
    uint16_t retransmit_count;
    enum esb_tx_mode tx_mode;
    uint8_t payload_length;
    bool selective_auto_ack;
    bool use_fast_ramp_up;
};

int esb_init(const struct esb_config *config);
bool esb_is_idle(void);
int esb_write_payload(const struct esb_payload *payload);
int esb_read_rx_payload(struct esb_payload *payload);
int esb_start_rx(void);
int esb_stop_rx(void);
int esb_flush_tx(void);
int esb_set_address_length(uint8_t length);
int esb_set_base_address_0(const uint8_t *addr);
int esb_set_base_address_1(const uint8_t *addr);
int esb_set_prefixes(const uint8_t *prefixes, uint8_t num_pipes);
int esb_set_rf_channel(uint32_t channel);
int esb_set_tx_power(int8_t tx_output_power);
int esb_set_retransmit_delay(uint16_t delay);
int esb_set_retransmit_count(uint16_t count);
