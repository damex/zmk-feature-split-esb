// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#define COBS_MAX_ENCODED(input_length) ((input_length) + (input_length) / 254 + 2)

int cobs_encode(const uint8_t *input, size_t input_length,
                uint8_t *output, size_t output_capacity);

int cobs_decode_in_place(uint8_t *buffer, size_t buffer_length, size_t *output_length);
