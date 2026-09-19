// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "cobs.h"

#define TEST_COBS_MAX_INPUT 512

static void expect_roundtrip(const uint8_t *input, size_t input_length) {
    zassert_true(input_length <= TEST_COBS_MAX_INPUT, "test input fits helper buffer");
    uint8_t encoded[TEST_COBS_MAX_INPUT + 4];
    const int encoded_length = cobs_encode(input, input_length, encoded, sizeof(encoded));
    zassert_true(encoded_length > 0, "encode succeeded");
    zassert_true((size_t)encoded_length >= input_length + 2, "encoded length includes overhead");
    zassert_equal(encoded[encoded_length - 1], 0x00, "trailing delimiter present");
    for (int index = 0; index + 1 < encoded_length; index++) {
        zassert_not_equal(encoded[index], 0x00, "no zero inside encoded body");
    }
    uint8_t decode_buffer[TEST_COBS_MAX_INPUT + 4];
    memcpy(decode_buffer, encoded, (size_t)(encoded_length - 1));
    size_t decoded_length = 0;
    const int decode_result = cobs_decode_in_place(decode_buffer, (size_t)(encoded_length - 1),
                                                   &decoded_length);
    zassert_equal(decode_result, 0, "decode ok");
    zassert_equal(decoded_length, input_length, "decoded length matches");
    zassert_mem_equal(decode_buffer, input, input_length, "decoded bytes match");
}

ZTEST_SUITE(cobs, NULL, NULL, NULL, NULL, NULL);

ZTEST(cobs, test_roundtrip_single_nonzero) {
    const uint8_t input[] = {0x42};
    expect_roundtrip(input, sizeof(input));
}

ZTEST(cobs, test_roundtrip_single_zero) {
    const uint8_t input[] = {0x00};
    expect_roundtrip(input, sizeof(input));
}

ZTEST(cobs, test_roundtrip_mixed_zeros) {
    const uint8_t input[] = {0x00, 0xAA, 0x00, 0xBB, 0x00, 0x00};
    expect_roundtrip(input, sizeof(input));
}

ZTEST(cobs, test_roundtrip_all_zeros) {
    uint8_t input[16];
    memset(input, 0x00, sizeof(input));
    expect_roundtrip(input, sizeof(input));
}

ZTEST(cobs, test_roundtrip_all_nonzero_under_254) {
    uint8_t input[100];
    for (size_t index = 0; index < sizeof(input); index++) {
        input[index] = (uint8_t)((index & 0x7F) | 0x01);
    }
    expect_roundtrip(input, sizeof(input));
}

ZTEST(cobs, test_roundtrip_across_254_boundary) {
    uint8_t input[300];
    for (size_t index = 0; index < sizeof(input); index++) {
        input[index] = (uint8_t)((index % 250) + 1);
    }
    expect_roundtrip(input, sizeof(input));
}

ZTEST(cobs, test_encode_rejects_undersized_output) {
    const uint8_t input[] = {0x11, 0x22, 0x33};
    uint8_t output[3];
    const int encoded_length = cobs_encode(input, sizeof(input), output, sizeof(output));
    zassert_equal(encoded_length, -ENOMEM, "encode refuses when output too small");
}

ZTEST(cobs, test_decode_rejects_zero_code_byte) {
    uint8_t buffer[] = {0x00, 0xAB, 0xCD};
    size_t decoded_length = 0;
    const int result = cobs_decode_in_place(buffer, sizeof(buffer), &decoded_length);
    zassert_equal(result, -EINVAL, "zero code byte rejected");
}

ZTEST(cobs, test_decode_rejects_code_beyond_input) {
    uint8_t buffer[] = {0x05, 0xAA};
    size_t decoded_length = 0;
    const int result = cobs_decode_in_place(buffer, sizeof(buffer), &decoded_length);
    zassert_equal(result, -EINVAL, "code overrun rejected");
}

ZTEST(cobs, test_decode_rejects_empty_buffer) {
    uint8_t buffer[1];
    size_t decoded_length = 0;
    const int result = cobs_decode_in_place(buffer, 0, &decoded_length);
    zassert_equal(result, -EINVAL, "empty buffer rejected");
}

ZTEST(cobs, test_encode_rejects_at_254_boundary) {
    uint8_t input[254];
    memset(input, 0x11, sizeof(input));
    uint8_t output[sizeof(input) + 2];
    const int encoded_length = cobs_encode(input, sizeof(input), output, sizeof(output));
    zassert_equal(encoded_length, -ENOMEM, "254 non-zero bytes need input+3, not input+2");
}

ZTEST(cobs, test_roundtrip_at_254_boundary) {
    uint8_t input[254];
    memset(input, 0x11, sizeof(input));
    expect_roundtrip(input, sizeof(input));
}
