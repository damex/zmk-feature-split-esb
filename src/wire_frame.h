// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#define WIRE_FRAME_MAX_PAYLOAD        128
#define WIRE_FRAME_CRC_BYTES          1
#define WIRE_FRAME_COBS_OVERHEAD_MAX  2
#define WIRE_FRAME_MAX_ENCODED        (WIRE_FRAME_MAX_PAYLOAD + WIRE_FRAME_CRC_BYTES + \
                                       WIRE_FRAME_COBS_OVERHEAD_MAX)

typedef void (*wire_frame_ingest_callback_t)(const uint8_t *payload, size_t length,
                                             void *user_data);

struct wire_frame_parser {
    uint8_t accumulator[WIRE_FRAME_MAX_ENCODED];
    size_t accumulator_length;
};

void wire_frame_parser_ingest(struct wire_frame_parser *parser,
                              const uint8_t *bytes, size_t length,
                              wire_frame_ingest_callback_t callback, void *user_data);

int wire_frame_encode(const uint8_t *payload, size_t payload_length,
                      uint8_t *output, size_t output_capacity);
