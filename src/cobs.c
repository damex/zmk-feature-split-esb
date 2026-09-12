// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include "cobs.h"

#include <errno.h>

int cobs_encode(const uint8_t *input, size_t input_length,
                uint8_t *output, size_t output_capacity) {
    if (output_capacity < input_length + 2) {
        return -ENOMEM;
    }
    size_t output_index = 1;
    size_t code_position = 0;
    uint8_t code = 1;
    for (size_t input_index = 0; input_index < input_length; input_index++) {
        if (input[input_index] == 0) {
            output[code_position] = code;
            code_position = output_index++;
            code = 1;
        } else {
            output[output_index++] = input[input_index];
            code++;
            if (code == 0xFF) {
                output[code_position] = code;
                code_position = output_index++;
                code = 1;
            }
        }
    }
    output[code_position] = code;
    output[output_index++] = 0x00;
    return (int)output_index;
}

int cobs_decode_in_place(uint8_t *buffer, size_t buffer_length, size_t *output_length) {
    if (buffer_length < 1) {
        return -EINVAL;
    }
    size_t input_index = 0;
    size_t output_index = 0;
    while (input_index < buffer_length) {
        uint8_t code = buffer[input_index++];
        if (code == 0 || (size_t)(code - 1) > buffer_length - input_index) {
            return -EINVAL;
        }
        for (uint8_t byte_index = 1; byte_index < code; byte_index++) {
            buffer[output_index++] = buffer[input_index++];
        }
        if (code < 0xFF && input_index < buffer_length) {
            buffer[output_index++] = 0x00;
        }
    }
    *output_length = output_index;
    return 0;
}
