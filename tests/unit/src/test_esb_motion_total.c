// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include <zephyr/ztest.h>

#include "esb_motion_total.h"

#define HOP_WINDOW_MS 32

ZTEST_SUITE(esb_motion_total, NULL, NULL, NULL, NULL, NULL);

ZTEST(esb_motion_total, test_add_wraps) {
    zassert_equal(esb_motion_total_add(5, -7), -2, "adds signed motion");
    zassert_equal(esb_motion_total_add(INT32_MAX, 1), INT32_MIN, "wraps past the top");
    zassert_equal(esb_motion_total_add(INT32_MIN, -1), INT32_MAX, "wraps past the bottom");
}

ZTEST(esb_motion_total, test_first_total_only_adopts) {
    struct esb_motion_track track = {0};
    int32_t delta = 0;

    zassert_false(esb_motion_track_delta(&track, 40, &delta),
                  "first total adopts the baseline, forwards nothing");
    zassert_true(esb_motion_track_delta(&track, 43, &delta), "second total forwards");
    zassert_equal(delta, 3, "delta is the difference");
}

ZTEST(esb_motion_total, test_lost_packet_heals) {
    struct esb_motion_track track = {0};
    int32_t delta = 0;

    (void)esb_motion_track_delta(&track, 0, &delta);
    zassert_true(esb_motion_track_delta(&track, 10, &delta), "total after lost packets forwards");
    zassert_equal(delta, 10, "lost motion arrives with the next total");
}

ZTEST(esb_motion_total, test_repeat_forwards_nothing) {
    struct esb_motion_track track = {0};
    int32_t delta = 0;

    (void)esb_motion_track_delta(&track, 7, &delta);
    zassert_false(esb_motion_track_delta(&track, 7, &delta), "same total forwards nothing");
}

ZTEST(esb_motion_total, test_delta_across_wrap) {
    struct esb_motion_track track = {0};
    int32_t delta = 0;

    (void)esb_motion_track_delta(&track, INT32_MAX - 1, &delta);
    zassert_true(esb_motion_track_delta(&track, INT32_MIN + 1, &delta), "total past the wrap");
    zassert_equal(delta, 3, "delta crosses the int32 wrap");
}

ZTEST(esb_motion_total, test_gap_past_cap_resyncs) {
    struct esb_motion_track track = {0};
    int32_t delta = 0;

    (void)esb_motion_track_delta(&track, 0, &delta);
    zassert_true(esb_motion_track_delta(&track, ESB_MOTION_RESYNC_COUNTS, &delta),
                 "gap at the cap forwards");
    zassert_false(esb_motion_track_delta(&track, -1, &delta),
                  "gap past the cap re-baselines");
    zassert_true(esb_motion_track_delta(&track, 1, &delta), "next total forwards");
    zassert_equal(delta, 2, "from the new baseline");
}

ZTEST(esb_motion_total, test_short_total_needs_baseline) {
    struct esb_motion_track track = {0};
    int32_t delta = 0;

    zassert_false(esb_motion_track_short_delta(&track, 5, &delta),
                  "short total without a baseline forwards nothing");
    zassert_false(track.valid, "short total sets no baseline");
}

ZTEST(esb_motion_total, test_short_total_heals_tail) {
    struct esb_motion_track track = {.last_total = 70000, .valid = true};
    int32_t delta = 0;

    zassert_true(esb_motion_track_short_delta(&track, (int16_t)70006, &delta),
                 "short total ahead of the baseline forwards");
    zassert_equal(delta, 6, "delta from the low 16 bits");
    zassert_equal(track.last_total, 70006, "baseline keeps its high bits");
    zassert_false(esb_motion_track_short_delta(&track, (int16_t)70006, &delta),
                  "same short total forwards nothing");
}

ZTEST(esb_motion_total, test_quiet_rule) {
    zassert_false(esb_motion_quiet_enough(HOP_WINDOW_MS - 1, HOP_WINDOW_MS),
                  "under a hop window of quiet: keepalive total is stale");
    zassert_true(esb_motion_quiet_enough(HOP_WINDOW_MS, HOP_WINDOW_MS),
                 "a hop window of quiet: keepalive total applies");
    zassert_true(esb_motion_quiet_enough(UINT32_MAX, HOP_WINDOW_MS), "no overflow at type max");
}
