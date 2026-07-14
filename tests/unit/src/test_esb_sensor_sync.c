// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <zephyr/ztest.h>

#include "esb_sensor_sync.h"

#define DETENT_UDEG (18 * ESB_SENSOR_MICRODEG_PER_DEG)

ZTEST_SUITE(esb_sensor_sync, NULL, NULL, NULL, NULL, NULL);

ZTEST(esb_sensor_sync, test_udeg_split_roundtrip) {
    int64_t udeg = esb_sensor_udeg(-3, -500000);

    zassert_equal(udeg, -3500000LL, "negative val1/val2 fold into one total");
    zassert_equal(esb_sensor_udeg_val1(udeg), -3, "val1 recovers whole degrees");
    zassert_equal(esb_sensor_udeg_val2(udeg), -500000, "val2 recovers microdegrees");
}

ZTEST(esb_sensor_sync, test_first_total_only_adopts) {
    struct esb_sensor_track track = {0};
    int64_t delta_udeg = 0;

    zassert_false(esb_sensor_track_delta(&track, 5 * DETENT_UDEG, &delta_udeg),
                  "first sample adopts baseline, forwards nothing");
    zassert_true(esb_sensor_track_delta(&track, 6 * DETENT_UDEG, &delta_udeg),
                 "second sample forwards");
    zassert_equal(delta_udeg, DETENT_UDEG, "delta is one detent");
}

ZTEST(esb_sensor_sync, test_lost_packet_heals) {
    struct esb_sensor_track track = {0};
    int64_t delta_udeg = 0;

    zassert_false(esb_sensor_track_delta(&track, 0, &delta_udeg),
                  "baseline at rest forwards nothing");
    zassert_true(esb_sensor_track_delta(&track, 3 * DETENT_UDEG, &delta_udeg),
                 "total after two lost packets forwards");
    zassert_equal(delta_udeg, 3 * DETENT_UDEG, "lost detents arrive with next total");
}

ZTEST(esb_sensor_sync, test_zero_delta_not_forwarded) {
    struct esb_sensor_track track = {0};
    int64_t delta_udeg = 0;

    zassert_false(esb_sensor_track_delta(&track, DETENT_UDEG, &delta_udeg),
                  "first sample adopts");
    zassert_false(esb_sensor_track_delta(&track, DETENT_UDEG, &delta_udeg),
                  "repeated total forwards nothing");
}

ZTEST(esb_sensor_sync, test_reboot_gap_resyncs) {
    struct esb_sensor_track track = {0};
    int64_t delta_udeg = 0;

    zassert_false(esb_sensor_track_delta(&track, 40 * DETENT_UDEG, &delta_udeg),
                  "first sample adopts");
    zassert_false(esb_sensor_track_delta(&track, DETENT_UDEG, &delta_udeg),
                  "gap past the cap resyncs instead of replaying");
    zassert_true(esb_sensor_track_delta(&track, 2 * DETENT_UDEG, &delta_udeg),
                 "next total forwards from the new baseline");
    zassert_equal(delta_udeg, DETENT_UDEG, "delta measured from resynced baseline");
}

ZTEST(esb_sensor_sync, test_ccw_delta_forwards_negative) {
    struct esb_sensor_track track = {0};
    int64_t delta_udeg = 0;

    zassert_false(esb_sensor_track_delta(&track, 4 * DETENT_UDEG, &delta_udeg),
                  "first sample adopts");
    zassert_true(esb_sensor_track_delta(&track, 3 * DETENT_UDEG, &delta_udeg),
                 "ccw total forwards");
    zassert_equal(delta_udeg, -DETENT_UDEG, "delta is one detent ccw");
}
