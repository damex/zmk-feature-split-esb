// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "esb_keepalive.h"

ZTEST_SUITE(esb_keepalive, NULL, NULL, NULL, NULL, NULL);

ZTEST(esb_keepalive, test_encode_layout) {
    uint8_t bitmap[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    esb_keepalive_bitmap_set(bitmap, 0, true);
    esb_keepalive_bitmap_set(bitmap, 63, true);
    uint8_t wire[ESB_KEEPALIVE_LENGTH];
    esb_keepalive_encode(wire, 0x01, bitmap, 97);
    zassert_equal(wire[ESB_KEEPALIVE_TAG_OFFSET], ESB_KEEPALIVE_TAG, "tag byte");
    zassert_equal(esb_keepalive_state(wire), 0x01, "state byte");
    zassert_mem_equal(esb_keepalive_bitmap(wire), bitmap, ESB_KEEPALIVE_BITMAP_BYTES, "bitmap");
    zassert_equal(esb_keepalive_battery_level(wire), 97, "battery byte");
}

ZTEST(esb_keepalive, test_matches) {
    uint8_t bitmap[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    uint8_t wire[ESB_KEEPALIVE_LENGTH];
    esb_keepalive_encode(wire, 0x00, bitmap, ESB_KEEPALIVE_BATTERY_UNKNOWN);
    zassert_true(esb_keepalive_matches(wire, ESB_KEEPALIVE_LENGTH), "tagged full-length packet");
    zassert_false(esb_keepalive_matches(wire, 1), "beacon length is not a keepalive");
    uint8_t event_like[ESB_KEEPALIVE_LENGTH] = {0x02};
    zassert_false(esb_keepalive_matches(event_like, ESB_KEEPALIVE_LENGTH),
                  "event tag is not a keepalive");
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

ZTEST(esb_keepalive, test_bitmap_bounds) {
    uint8_t bitmap[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    esb_keepalive_bitmap_set(bitmap, ESB_KEEPALIVE_POSITION_COUNT, true);
    esb_keepalive_bitmap_set(bitmap, UINT32_MAX, true);
    uint8_t zeros[ESB_KEEPALIVE_BITMAP_BYTES] = {0};
    zassert_mem_equal(bitmap, zeros, sizeof(bitmap), "out-of-range set ignored");
    zassert_false(esb_keepalive_bitmap_get(bitmap, ESB_KEEPALIVE_POSITION_COUNT),
                  "out-of-range get reads false");
}
