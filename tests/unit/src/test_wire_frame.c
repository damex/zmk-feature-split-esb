// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "wire_frame.h"

#define JUNK_BYTE 0x55

struct delivery_log {
    size_t count;
    size_t length;
    uint8_t payload[WIRE_FRAME_MAX_PAYLOAD];
};

static void record_delivery(const uint8_t *payload, size_t length, void *user_data) {
    struct delivery_log *log = user_data;
    log->count++;
    log->length = length;
    memcpy(log->payload, payload, length);
}

static size_t encode_frame(const uint8_t *payload, size_t length, uint8_t *frame,
                           size_t frame_size) {
    int encoded = wire_frame_encode(payload, length, frame, frame_size);
    zassert_true(encoded > 0, "frame encodes");
    return (size_t)encoded;
}

ZTEST_SUITE(wire_frame, NULL, NULL, NULL, NULL, NULL);

ZTEST(wire_frame, test_frame_roundtrip) {
    const uint8_t payload[] = {0x11, 0x00, 0x22};
    uint8_t frame[WIRE_FRAME_MAX_ENCODED];
    size_t length = encode_frame(payload, sizeof(payload), frame, sizeof(frame));
    struct wire_frame_parser parser = {0};
    struct delivery_log log = {0};
    wire_frame_parser_ingest(&parser, frame, length, record_delivery, &log);
    zassert_equal(log.count, 1, "one frame delivered");
    zassert_equal(log.length, sizeof(payload), "payload length kept");
    zassert_mem_equal(log.payload, payload, sizeof(payload), "payload bytes kept");
}

ZTEST(wire_frame, test_frame_empty_payload_delivers_zero_length) {
    uint8_t frame[WIRE_FRAME_MAX_ENCODED];
    size_t length = encode_frame(NULL, 0, frame, sizeof(frame));
    struct wire_frame_parser parser = {0};
    struct delivery_log log = {0};
    wire_frame_parser_ingest(&parser, frame, length, record_delivery, &log);
    zassert_equal(log.count, 1, "keepalive frame delivered");
    zassert_equal(log.length, 0, "keepalive frame is empty");
}

ZTEST(wire_frame, test_frame_bad_crc_dropped) {
    const uint8_t payload[] = {0x11, 0x22};
    uint8_t frame[WIRE_FRAME_MAX_ENCODED];
    size_t length = encode_frame(payload, sizeof(payload), frame, sizeof(frame));
    frame[1] ^= 0x01;
    struct wire_frame_parser parser = {0};
    struct delivery_log log = {0};
    wire_frame_parser_ingest(&parser, frame, length, record_delivery, &log);
    zassert_equal(log.count, 0, "corrupted frame dropped");
}

ZTEST(wire_frame, test_frame_overflow_discards_until_delimiter) {
    const uint8_t tail[] = {0x11, 0x22};
    const uint8_t next[] = {0x33, 0x44};
    uint8_t junk[WIRE_FRAME_MAX_ENCODED + 1];
    memset(junk, JUNK_BYTE, sizeof(junk));
    uint8_t tail_frame[WIRE_FRAME_MAX_ENCODED];
    size_t tail_length = encode_frame(tail, sizeof(tail), tail_frame, sizeof(tail_frame));
    uint8_t next_frame[WIRE_FRAME_MAX_ENCODED];
    size_t next_length = encode_frame(next, sizeof(next), next_frame, sizeof(next_frame));
    struct wire_frame_parser parser = {0};
    struct delivery_log log = {0};
    wire_frame_parser_ingest(&parser, junk, sizeof(junk), record_delivery, &log);
    wire_frame_parser_ingest(&parser, tail_frame, tail_length, record_delivery, &log);
    zassert_equal(log.count, 0, "oversized frame tail never delivers");
    wire_frame_parser_ingest(&parser, next_frame, next_length, record_delivery, &log);
    zassert_equal(log.count, 1, "next frame after the delimiter delivers");
    zassert_mem_equal(log.payload, next, sizeof(next), "next frame payload kept");
}
