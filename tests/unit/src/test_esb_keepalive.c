// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "esb_keepalive.h"

ZTEST_SUITE(esb_keepalive, NULL, NULL, NULL, NULL, NULL);

static const uint8_t no_positions[ESB_KEEPALIVE_BITMAP_BYTES];

ZTEST(esb_keepalive, test_encode_layout) {
    uint8_t bitmap[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    esb_keepalive_bitmap_set(bitmap, 0, true);
    esb_keepalive_bitmap_set(bitmap, 63, true);
    const struct esb_keepalive_snapshot snapshot = {
        .state = 0x01,
        .position_bitmap = bitmap,
        .battery_level = 97,
        .link_cost_x10 = 23,
    };
    uint8_t wire[ESB_KEEPALIVE_LENGTH(0, 0)];
    esb_keepalive_encode(wire, sizeof(wire), &snapshot);
    zassert_equal(wire[ESB_KEEPALIVE_TAG_OFFSET], ESB_KEEPALIVE_TAG, "tag byte");
    zassert_equal(esb_keepalive_state(wire), 0x01, "state byte");
    zassert_mem_equal(esb_keepalive_bitmap(wire), bitmap, ESB_KEEPALIVE_BITMAP_BYTES, "bitmap");
    zassert_equal(esb_keepalive_battery_level(wire), 97, "battery byte");
    zassert_equal(esb_keepalive_link_cost_x10(wire), 23, "link cost byte");
    zassert_equal(esb_keepalive_held_count(wire), 0, "no held input keys");
}

ZTEST(esb_keepalive, test_encode_length) {
    int64_t totals[1] = {0};
    const struct esb_keepalive_snapshot snapshot = {
        .position_bitmap = no_positions,
        .sensor_totals_udeg = totals,
        .sensor_count = 1,
    };
    uint8_t wire[ESB_KEEPALIVE_LENGTH(0, 1)];
    size_t length = esb_keepalive_encode(wire, sizeof(wire), &snapshot);
    zassert_equal(length, ESB_KEEPALIVE_LENGTH(0, 1), "full buffer returns encoded length");
}

ZTEST(esb_keepalive, test_encode_short_buffer) {
    int64_t totals[1] = {0};
    const struct esb_keepalive_snapshot snapshot = {
        .position_bitmap = no_positions,
        .sensor_totals_udeg = totals,
        .sensor_count = 1,
    };
    uint8_t wire[ESB_KEEPALIVE_LENGTH(0, 1)];
    uint8_t untouched[sizeof(wire)];
    memset(wire, 0xAA, sizeof(wire));
    memcpy(untouched, wire, sizeof(untouched));
    size_t length = esb_keepalive_encode(wire, sizeof(wire) - 1, &snapshot);
    zassert_equal(length, 0, "short buffer returns 0");
    zassert_mem_equal(wire, untouched, sizeof(wire), "short buffer left unwritten");
}

ZTEST(esb_keepalive, test_matches) {
    const struct esb_keepalive_snapshot snapshot = {
        .position_bitmap = no_positions,
        .battery_level = ESB_KEEPALIVE_BATTERY_UNKNOWN,
    };
    uint8_t wire[ESB_KEEPALIVE_LENGTH(0, 0)];
    esb_keepalive_encode(wire, sizeof(wire), &snapshot);
    zassert_true(esb_keepalive_matches(wire, ESB_KEEPALIVE_LENGTH(0, 0)),
                 "tagged base-length packet");
    zassert_false(esb_keepalive_matches(wire, 1), "beacon length is not a keepalive");
    uint8_t event_like[ESB_KEEPALIVE_LENGTH(0, 0)] = {0x02};
    zassert_false(esb_keepalive_matches(event_like, ESB_KEEPALIVE_LENGTH(0, 0)),
                  "event tag is not a keepalive");
}

ZTEST(esb_keepalive, test_sensor_totals) {
    int64_t totals[2] = {90000000LL, -3500000LL};
    const struct esb_keepalive_snapshot snapshot = {
        .position_bitmap = no_positions,
        .sensor_totals_udeg = totals,
        .sensor_count = 2,
    };
    uint8_t wire[ESB_KEEPALIVE_LENGTH(0, 2)];
    esb_keepalive_encode(wire, sizeof(wire), &snapshot);
    zassert_true(esb_keepalive_matches(wire, ESB_KEEPALIVE_LENGTH(0, 2)), "totals length matches");
    zassert_false(esb_keepalive_matches(wire, ESB_KEEPALIVE_LENGTH(0, 2) - 1),
                  "partial total is not a keepalive");
    zassert_equal(esb_keepalive_sensor_count(wire, ESB_KEEPALIVE_LENGTH(0, 2)), 2,
                  "count from length");
    zassert_equal(esb_keepalive_sensor_count(wire, ESB_KEEPALIVE_LENGTH(0, 0)), 0,
                  "base length count");
    zassert_equal(esb_keepalive_sensor_total_udeg(wire, 0), 90000000LL, "first total");
    zassert_equal(esb_keepalive_sensor_total_udeg(wire, 1), -3500000LL, "negative total");
}

ZTEST(esb_keepalive, test_held_keys) {
    const struct esb_keepalive_held_key held[] = {
        {.reg = 1, .code = 0x110},
        {.reg = 2, .code = 0x14A},
    };
    int64_t totals[1] = {-1000000LL};
    const struct esb_keepalive_snapshot snapshot = {
        .position_bitmap = no_positions,
        .held_keys = held,
        .held_count = 2,
        .sensor_totals_udeg = totals,
        .sensor_count = 1,
    };
    uint8_t wire[ESB_KEEPALIVE_LENGTH(2, 1)];
    size_t length = esb_keepalive_encode(wire, sizeof(wire), &snapshot);
    zassert_equal(length, ESB_KEEPALIVE_LENGTH(2, 1), "held keys and totals length");
    zassert_true(esb_keepalive_matches(wire, length), "held keys packet matches");
    zassert_equal(esb_keepalive_held_count(wire), 2, "held count");
    struct esb_keepalive_held_key first = esb_keepalive_held_key_at(wire, 0);
    struct esb_keepalive_held_key second = esb_keepalive_held_key_at(wire, 1);
    zassert_equal(first.reg, 1, "first held reg");
    zassert_equal(first.code, 0x110, "first held code");
    zassert_equal(second.reg, 2, "second held reg");
    zassert_equal(second.code, 0x14A, "second held code");
    zassert_equal(esb_keepalive_sensor_count(wire, length), 1, "totals after held keys");
    zassert_equal(esb_keepalive_sensor_total_udeg(wire, 0), -1000000LL, "total after held keys");
}

ZTEST(esb_keepalive, test_held_keys_uncapped) {
    const struct esb_keepalive_held_key held[6] = {0};
    const struct esb_keepalive_snapshot snapshot = {
        .position_bitmap = no_positions,
        .held_keys = held,
        .held_count = ARRAY_SIZE(held),
    };
    uint8_t wire[ESB_KEEPALIVE_LENGTH(ARRAY_SIZE(held), 0)];
    size_t length = esb_keepalive_encode(wire, sizeof(wire), &snapshot);
    zassert_equal(length, sizeof(wire), "any held count that fits encodes");
    zassert_true(esb_keepalive_matches(wire, length), "any held count that fits matches");
}

ZTEST(esb_keepalive, test_held_count_past_packet_rejected) {
    const struct esb_keepalive_held_key held[1] = {0};
    const struct esb_keepalive_snapshot snapshot = {
        .position_bitmap = no_positions,
        .held_keys = held,
        .held_count = ARRAY_SIZE(held),
    };
    uint8_t wire[ESB_KEEPALIVE_LENGTH(ARRAY_SIZE(held), 0)];
    size_t length = esb_keepalive_encode(wire, sizeof(wire), &snapshot);
    zassert_false(esb_keepalive_matches(wire, length - 1), "truncated held key is rejected");
    wire[ESB_KEEPALIVE_HELD_COUNT_OFFSET] = 2;
    zassert_false(esb_keepalive_matches(wire, length), "count past the packet is rejected");
}

ZTEST(esb_keepalive, test_peripheral_state_active_only_while_searching) {
    zassert_equal(esb_keepalive_peripheral_state(true, true), ESB_KEEPALIVE_ACTIVE,
                  "active and searching reports active");
    zassert_equal(esb_keepalive_peripheral_state(true, false), ESB_KEEPALIVE_IDLE,
                  "active on a live link reports idle");
    zassert_equal(esb_keepalive_peripheral_state(false, true), ESB_KEEPALIVE_IDLE,
                  "idle while searching reports idle");
    zassert_equal(esb_keepalive_peripheral_state(false, false), ESB_KEEPALIVE_IDLE,
                  "idle on a live link reports idle");
}

ZTEST(esb_keepalive, test_bitmap_set_get_clear) {
    uint8_t bitmap[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    zassert_false(esb_keepalive_bitmap_get(bitmap, 5), "starts clear");
    esb_keepalive_bitmap_set(bitmap, 5, true);
    zassert_true(esb_keepalive_bitmap_get(bitmap, 5), "set reads back");
    zassert_false(esb_keepalive_bitmap_get(bitmap, 4), "neighbor untouched");
    zassert_false(esb_keepalive_bitmap_get(bitmap, 6), "neighbor untouched");
    esb_keepalive_bitmap_set(bitmap, 5, false);
    zassert_false(esb_keepalive_bitmap_get(bitmap, 5), "clear reads back");
}

ZTEST(esb_keepalive, test_key_verdict) {
    uint8_t tracked[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    zassert_equal(esb_keepalive_key_verdict(tracked, 5, true), ESB_KEEPALIVE_KEY_FORWARD,
                  "fresh press forwards");
    zassert_equal(esb_keepalive_key_verdict(tracked, 5, false),
                  ESB_KEEPALIVE_KEY_DROP_ORPHAN_RELEASE, "orphan release drops");
    esb_keepalive_bitmap_set(tracked, 5, true);
    zassert_equal(esb_keepalive_key_verdict(tracked, 5, true),
                  ESB_KEEPALIVE_KEY_HEAL_LOST_RELEASE, "repeated press heals");
    zassert_equal(esb_keepalive_key_verdict(tracked, 5, false), ESB_KEEPALIVE_KEY_FORWARD,
                  "matched release forwards");
}

ZTEST(esb_keepalive, test_bitmap_diff_next) {
    uint8_t tracked[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    uint8_t received[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    zassert_equal(esb_keepalive_bitmap_diff_next(tracked, received, 0),
                  ESB_KEEPALIVE_POSITION_COUNT, "identical bitmaps");
    esb_keepalive_bitmap_set(received, 3, true);
    esb_keepalive_bitmap_set(received, 40, true);
    esb_keepalive_bitmap_set(tracked, 63, true);
    zassert_equal(esb_keepalive_bitmap_diff_next(tracked, received, 0), 3, "first diff");
    zassert_equal(esb_keepalive_bitmap_diff_next(tracked, received, 4), 40, "second diff");
    zassert_equal(esb_keepalive_bitmap_diff_next(tracked, received, 41), 63,
                  "tracked-only diff");
    zassert_equal(esb_keepalive_bitmap_diff_next(tracked, received, 64),
                  ESB_KEEPALIVE_POSITION_COUNT, "scan from beyond bitmap");
}

ZTEST(esb_keepalive, test_bitmap_bounds) {
    uint8_t bitmap[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    esb_keepalive_bitmap_set(bitmap, ESB_KEEPALIVE_POSITION_COUNT, true);
    esb_keepalive_bitmap_set(bitmap, UINT32_MAX, true);
    uint8_t zeros[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    zassert_mem_equal(bitmap, zeros, sizeof(bitmap), "out-of-range set ignored");
    zassert_false(esb_keepalive_bitmap_get(bitmap, ESB_KEEPALIVE_POSITION_COUNT),
                  "out-of-range get reads false");
}
