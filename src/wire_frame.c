// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include "wire_frame.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/crc.h>

#include "cobs.h"

static void deliver_frame(struct wire_frame_parser *parser,
                          wire_frame_ingest_callback_t callback, void *user_data) {
    size_t decoded_length = 0;
    if (cobs_decode_in_place(parser->accumulator, parser->accumulator_length,
                             &decoded_length) != 0) {
        return;
    }
    if (decoded_length < 1) {
        return;
    }
    const size_t payload_length = decoded_length - 1;
    const uint8_t received_crc = parser->accumulator[decoded_length - 1];
    const uint8_t computed_crc = crc8_ccitt(0x00, parser->accumulator, payload_length);
    if (received_crc != computed_crc) {
        return;
    }
    if (callback != NULL) {
        callback(parser->accumulator, payload_length, user_data);
    }
}

void wire_frame_parser_ingest(struct wire_frame_parser *parser,
                              const uint8_t *bytes, size_t length,
                              wire_frame_ingest_callback_t callback, void *user_data) {
    for (size_t index = 0; index < length; index++) {
        const uint8_t byte = bytes[index];
        if (byte == 0x00) {
            if (parser->accumulator_length > 0) {
                deliver_frame(parser, callback, user_data);
                parser->accumulator_length = 0;
            }
            continue;
        }
        if (parser->accumulator_length >= sizeof(parser->accumulator)) {
            parser->accumulator_length = 0;
            continue;
        }
        parser->accumulator[parser->accumulator_length++] = byte;
    }
}

int wire_frame_encode(const uint8_t *payload, size_t payload_length,
                      uint8_t *output, size_t output_capacity) {
    if (payload_length > WIRE_FRAME_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    if (payload_length > 0 && payload == NULL) {
        return -EINVAL;
    }
    uint8_t plain[WIRE_FRAME_MAX_PAYLOAD + WIRE_FRAME_CRC_BYTES];
    if (payload_length == 0) {
        plain[0] = 0;
    } else {
        memcpy(plain, payload, payload_length);
        plain[payload_length] = crc8_ccitt(0x00, payload, payload_length);
    }
    return cobs_encode(plain, payload_length + WIRE_FRAME_CRC_BYTES, output, output_capacity);
}
