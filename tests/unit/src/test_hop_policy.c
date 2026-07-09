// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "hop_policy.h"

#define ANCHOR_POOL_MAX 101
#define ANCHOR_COUNT_DEFAULT 3

ZTEST_SUITE(hop_policy, NULL, NULL, NULL, NULL, NULL);

ZTEST(hop_policy, test_saturating_add) {
    zassert_equal(hop_policy_saturating_add(0, 0), 0, NULL);
    zassert_equal(hop_policy_saturating_add(10, 5), 15, "adds");
    zassert_equal(hop_policy_saturating_add(250, 5), 255, "reaches max exactly");
    zassert_equal(hop_policy_saturating_add(250, 10), UINT8_MAX, "saturates, no wrap");
    zassert_equal(hop_policy_saturating_add(UINT8_MAX, 1), UINT8_MAX, "stays at max");
}

ZTEST(hop_policy, test_should_hop_threshold) {
    uint8_t bad_windows = 0;
    const uint16_t threshold = 3;
    zassert_false(hop_policy_should_hop(&bad_windows, 1, threshold), "1 of 3");
    zassert_false(hop_policy_should_hop(&bad_windows, 1, threshold), "2 of 3");
    zassert_true(hop_policy_should_hop(&bad_windows, 1, threshold), "3rd consecutive hops");
    zassert_equal(bad_windows, 0, "streak clears after hop");
}

ZTEST(hop_policy, test_should_hop_good_window_resets) {
    uint8_t bad_windows = 0;
    const uint16_t threshold = 3;
    hop_policy_should_hop(&bad_windows, 1, threshold);
    hop_policy_should_hop(&bad_windows, 1, threshold);
    zassert_false(hop_policy_should_hop(&bad_windows, 0, threshold), "good window, no hop");
    zassert_equal(bad_windows, 0, "good window clears the streak");
    zassert_false(hop_policy_should_hop(&bad_windows, 1, threshold), "streak restarts from 0");
}

ZTEST(hop_policy, test_should_hop_threshold_one) {
    uint8_t bad_windows = 0;
    zassert_true(hop_policy_should_hop(&bad_windows, 1, 1), "threshold 1 hops on first fail");
}

ZTEST(hop_policy, test_should_hop_graded) {
    uint8_t bad_windows = 0;
    /* a big penalty reaches the threshold in one window */
    zassert_true(hop_policy_should_hop(&bad_windows, 4, 3), "penalty over threshold hops at once");
    zassert_equal(bad_windows, 0, "clears after hop");
}

ZTEST(hop_policy, test_attempts_penalty) {
    const uint8_t good = 2;
    zassert_equal(hop_policy_attempts_penalty(1, good), 0, "first try: no penalty");
    zassert_equal(hop_policy_attempts_penalty(2, good), 0, "at good limit: no penalty");
    zassert_equal(hop_policy_attempts_penalty(3, good), 1, "just over: 1");
    zassert_equal(hop_policy_attempts_penalty(6, good), 2, "4 over: 2");
    zassert_equal(hop_policy_attempts_penalty(14, good), HOP_POLICY_MAX_LOSS_PENALTY, "many: capped");
    zassert_equal(hop_policy_attempts_penalty(255, good), HOP_POLICY_MAX_LOSS_PENALTY, "lost: capped");
}

ZTEST(hop_policy, test_is_beacon) {
    const uint8_t beacon[ESB_BEACON_LENGTH] = {[0] = ESB_BEACON_TAG};
    zassert_true(hop_policy_is_beacon(beacon, ESB_BEACON_LENGTH), "tagged, right length");
    const uint8_t command[ESB_BEACON_LENGTH] = {[0] = 0x01};
    zassert_false(hop_policy_is_beacon(command, ESB_BEACON_LENGTH), "wrong tag is not beacon");
    zassert_false(hop_policy_is_beacon(beacon, 2), "wrong length is not beacon");
}

ZTEST(hop_policy, test_index_next_wraps) {
    zassert_equal(hop_policy_index_next(0, 3), 1, NULL);
    zassert_equal(hop_policy_index_next(1, 3), 2, NULL);
    zassert_equal(hop_policy_index_next(2, 3), 0, "wraps at count");
    zassert_equal(hop_policy_index_next(0, 1), 0, "single channel stays put");
}

ZTEST(hop_policy, test_camp_step_holds_then_rotates) {
    uint8_t anchor = 2; /* ANCHOR_COUNT - 1, the peripheral's camp init */
    uint16_t dwell = 0;
    hop_policy_camp_step(&anchor, &dwell, 3, 4);
    zassert_equal(anchor, 0, "empty dwell rotates to first anchor");
    zassert_equal(dwell, 4, "reloads dwell on rotate");
    for (int window = 0; window < 4; window++) {
        hop_policy_camp_step(&anchor, &dwell, 3, 4);
        zassert_equal(anchor, 0, "anchor steady through the hold");
    }
    zassert_equal(dwell, 0, "dwell drains over the hold");
    hop_policy_camp_step(&anchor, &dwell, 3, 4);
    zassert_equal(anchor, 1, "rotates once the hold expires");
}

ZTEST(hop_policy, test_camp_step_single_anchor_stays) {
    uint8_t anchor = 0;
    uint16_t dwell = 0;
    hop_policy_camp_step(&anchor, &dwell, 1, 2);
    zassert_equal(anchor, 0, "lone anchor never leaves");
}

ZTEST(hop_policy, test_score_update_accrues_and_saturates) {
    uint8_t score = 0;
    hop_policy_score_update(&score, 3, 1);
    zassert_equal(score, 3, "penalty accrues");
    hop_policy_score_update(&score, 0, 1);
    zassert_equal(score, 2, "clean window decays one step");
    score = 254;
    hop_policy_score_update(&score, 5, 1);
    zassert_equal(score, 255, "saturates at UINT8_MAX");
    score = 0;
    hop_policy_score_update(&score, 0, 1);
    zassert_equal(score, 0, "decay floors at zero");
}

ZTEST(hop_policy, test_worst_channel) {
    uint8_t mask[1] = {0x3F};
    uint8_t anchors[1] = {0x03};
    uint8_t bad[6] = {99, 99, 20, 10, 30, 18};
    zassert_equal(hop_policy_worst_channel(bad, mask, anchors, 6, 16), 4, "anchors 0,1 exempt, worst is 4");
    uint8_t low[6] = {99, 99, 5, 5, 5, 5};
    zassert_equal(hop_policy_worst_channel(low, mask, anchors, 6, 16), 6, "none over threshold returns count");
    uint8_t masked[1] = {0x0F};
    zassert_equal(hop_policy_worst_channel(bad, masked, anchors, 6, 16), 2, "inactive channel 4 skipped");
}

ZTEST(hop_policy, test_worst_channel_tie_takes_lowest) {
    uint8_t mask[1] = {0x3F};
    uint8_t anchors[1] = {0x00};
    uint8_t tied[6] = {10, 30, 30, 10, 10, 10};
    zassert_equal(hop_policy_worst_channel(tied, mask, anchors, 6, 16), 1,
                  "equal scores keep the first channel found");
}

ZTEST(hop_policy, test_retest_threshold) {
    zassert_equal(hop_policy_retest_threshold(64, 0), 64, "level 0 is the base period");
    zassert_equal(hop_policy_retest_threshold(64, 1), 128, "each level doubles");
    zassert_equal(hop_policy_retest_threshold(64, 3), 512, "level 3 is 8x base");
    zassert_equal(hop_policy_retest_threshold(64, HOP_POLICY_RETEST_LEVEL_MAX + 5),
                  hop_policy_retest_threshold(64, HOP_POLICY_RETEST_LEVEL_MAX),
                  "level saturates at the max");
    zassert_equal(hop_policy_retest_threshold(60000, 4), UINT16_MAX, "overflow saturates");
    zassert_equal(hop_policy_retest_threshold(0, 5), 0, "zero base retests every window");
}

ZTEST(hop_policy, test_window_penalty) {
    int8_t rssi[2] = {-50, -50};
    zassert_equal(hop_policy_window_penalty(0x1, 0x3, rssi, -85, 2), HOP_POLICY_MAX_LOSS_PENALTY,
                  "active pipe with no motion scores max");
    zassert_equal(hop_policy_window_penalty(0, 0, rssi, -85, 2), 0, "no active pipe is clean");
    int8_t weak[1] = {-97};
    zassert_equal(hop_policy_window_penalty(0x1, 0x1, weak, -85, 1), 3, "12 dB under floor grades to 3");
}

ZTEST(hop_policy, test_mask_set_round_trip) {
    uint8_t mask[2] = {0};
    hop_policy_mask_set(mask, 3, true);
    hop_policy_mask_set(mask, 11, true);
    zassert_true(hop_policy_mask_get(mask, 3), NULL);
    zassert_true(hop_policy_mask_get(mask, 11), NULL);
    zassert_false(hop_policy_mask_get(mask, 4), NULL);
    hop_policy_mask_set(mask, 3, false);
    zassert_false(hop_policy_mask_get(mask, 3), "clears the bit");
}

ZTEST(hop_policy, test_channel_for_epoch) {
    zassert_equal(hop_policy_channel_for_epoch(0, 3), 0, NULL);
    zassert_equal(hop_policy_channel_for_epoch(1, 3), 1, NULL);
    zassert_equal(hop_policy_channel_for_epoch(3, 3), 0, "wraps");
    zassert_equal(hop_policy_channel_for_epoch(7, 3), 1, NULL);
    zassert_equal(hop_policy_channel_for_epoch(5, 1), 0, "single channel always 0");
}

ZTEST(hop_policy, test_anchor_default_index) {
    zassert_equal(hop_policy_anchor_default_index(0, 10, 3), 1, "pool of 10, first slice midpoint");
    zassert_equal(hop_policy_anchor_default_index(1, 10, 3), 5, "second slice midpoint");
    zassert_equal(hop_policy_anchor_default_index(2, 10, 3), 8, "third slice midpoint");
    zassert_equal(hop_policy_anchor_default_index(0, 1, 1), 0, "lone channel anchors itself");
    zassert_equal(hop_policy_anchor_default_index(0, 3, 3), 0, "anchors fill the pool");
    zassert_equal(hop_policy_anchor_default_index(2, 3, 3), 2, "last slot takes the last channel");
}

ZTEST(hop_policy, test_anchor_default_index_spreads) {
    for (size_t pool_count = 1; pool_count <= ANCHOR_POOL_MAX; pool_count++) {
        size_t anchor_count = MIN((size_t)ANCHOR_COUNT_DEFAULT, pool_count);
        uint8_t minimum_gap = (uint8_t)(pool_count / anchor_count);
        uint8_t previous = 0;
        for (size_t slot = 0; slot < anchor_count; slot++) {
            uint8_t index = hop_policy_anchor_default_index(slot, pool_count, anchor_count);
            zassert_true(index < pool_count, "anchor index stays inside the pool");
            if (slot > 0) {
                zassert_true(index > previous, "anchors never share a channel");
                zassert_true(index - previous >= minimum_gap, "anchors sit a slice apart");
            }
            previous = index;
        }
    }
}

ZTEST(hop_policy, test_mask_get_and_count) {
    const uint8_t mask[1] = {0x05}; /* bits 0 and 2 */
    zassert_true(hop_policy_mask_get(mask, 0), "bit 0 set");
    zassert_false(hop_policy_mask_get(mask, 1), "bit 1 clear");
    zassert_true(hop_policy_mask_get(mask, 2), "bit 2 set");
    zassert_equal(hop_policy_mask_active_count(mask, 4), 2, "two active in pool of 4");
}

ZTEST(hop_policy, test_channel_for_epoch_masked) {
    const uint8_t all[1] = {0x0F}; /* pool of 4, all active */
    zassert_equal(hop_policy_channel_for_epoch_masked(0, all, 4), 0, "all active is identity");
    zassert_equal(hop_policy_channel_for_epoch_masked(5, all, 4), 1, "epoch 5 -> base 1");

    const uint8_t two[1] = {0x05}; /* channels 0 and 2 active */
    zassert_equal(hop_policy_channel_for_epoch_masked(0, two, 4), 0, "base 0 active, stays");
    zassert_equal(hop_policy_channel_for_epoch_masked(1, two, 4), 2, "base 1 masked, probes to 2");
    zassert_equal(hop_policy_channel_for_epoch_masked(2, two, 4), 2, "base 2 active, stays");
    zassert_equal(hop_policy_channel_for_epoch_masked(3, two, 4), 0, "base 3 masked, probes wrap to 0");

    const uint8_t none[1] = {0x00};
    zassert_equal(hop_policy_channel_for_epoch_masked(1, none, 4),
                  hop_policy_channel_for_epoch(1, 4), "all masked falls back to base");
}

ZTEST(hop_policy, test_channel_for_epoch_masked_multibyte) {
    const uint8_t all[2] = {0xFF, 0x07};
    zassert_equal(hop_policy_channel_for_epoch_masked(8, all, 11), 8, "high channel active, stays");

    const uint8_t no_eight[2] = {0xFF, 0x06};
    zassert_equal(hop_policy_channel_for_epoch_masked(8, no_eight, 11), 9,
                  "masked high channel probes within the second byte");

    const uint8_t no_ten[2] = {0xFF, 0x03};
    zassert_equal(hop_policy_channel_for_epoch_masked(10, no_ten, 11), 0,
                  "masked last channel wraps to the first byte");
}

ZTEST(hop_policy, test_masked_mapping_stable) {
    const uint8_t all[1] = {0x0F};
    const uint8_t masked[1] = {0x0D}; /* channel 1 masked */
    zassert_equal(hop_policy_channel_for_epoch_masked(0, all, 4),
                  hop_policy_channel_for_epoch_masked(0, masked, 4), "epoch 0 unchanged");
    zassert_equal(hop_policy_channel_for_epoch_masked(2, all, 4),
                  hop_policy_channel_for_epoch_masked(2, masked, 4), "epoch 2 unchanged");
    zassert_equal(hop_policy_channel_for_epoch_masked(3, all, 4),
                  hop_policy_channel_for_epoch_masked(3, masked, 4), "epoch 3 unchanged");
    zassert_not_equal(hop_policy_channel_for_epoch_masked(1, all, 4),
                      hop_policy_channel_for_epoch_masked(1, masked, 4), "only epoch 1 redirected");
}

ZTEST(hop_policy, test_hop_vote) {
    const uint8_t weights[3] = {3, 1, 1};
    const uint16_t threshold = 6;
    uint8_t link_loss[3];

    link_loss[0] = 0; link_loss[1] = 0; link_loss[2] = 0;
    zassert_false(hop_policy_hop_vote(link_loss, weights, 3, threshold), "all good, no hop");

    link_loss[0] = 2; link_loss[1] = 0; link_loss[2] = 0;
    zassert_true(hop_policy_hop_vote(link_loss, weights, 3, threshold), "high-weight bad trips it");

    link_loss[0] = 0; link_loss[1] = 5; link_loss[2] = 0;
    zassert_false(hop_policy_hop_vote(link_loss, weights, 3, threshold),
                  "one low-weight under threshold");

    link_loss[0] = 0; link_loss[1] = 3; link_loss[2] = 3;
    zassert_true(hop_policy_hop_vote(link_loss, weights, 3, threshold),
                 "low-weight sum reaches threshold");
}

ZTEST(hop_policy, test_accrue_loss) {
    uint8_t link_loss[3] = {0, 0, 5};
    const int8_t rssi[3] = {-40, -97, 0}; /* pipe0 strong, pipe1 weak, pipe2 n/a */
    const int8_t floor = -85;
    /* pipe0 active+motion strong, pipe1 active+motion weak, pipe2 active with no motion */
    hop_policy_accrue_loss(link_loss, 3, (1u << 0) | (1u << 1),
                           (1u << 0) | (1u << 1) | (1u << 2), rssi, floor);
    zassert_equal(link_loss[0], 0, "strong motion clears");
    zassert_equal(link_loss[1], 3, "weak motion adds a graded penalty");
    zassert_equal(link_loss[2], 5 + HOP_POLICY_MAX_LOSS_PENALTY, "no motion adds the max");

    uint8_t idle[1] = {7};
    const int8_t strong[1] = {-40};
    hop_policy_accrue_loss(idle, 1, 0, 0, strong, floor); /* not active */
    zassert_equal(idle[0], 0, "idle pipe clears, never drives a hop");

    uint8_t sat[1] = {UINT8_MAX};
    const int8_t weak[1] = {-120};
    hop_policy_accrue_loss(sat, 1, (1u << 0), (1u << 0), weak, floor);
    zassert_equal(sat[0], UINT8_MAX, "loss saturates, no wrap");
}

ZTEST(hop_policy, test_rssi_to_dbm) {
    zassert_equal(hop_policy_rssi_to_dbm(42), -42, "magnitude to negative dBm");
    zassert_equal(hop_policy_rssi_to_dbm(0), 0, "zero stays zero");
    zassert_equal(hop_policy_rssi_to_dbm(95), -95, NULL);
}

ZTEST(hop_policy, test_keepalive_is_active) {
    zassert_true(hop_policy_keepalive_is_active(ESB_KEEPALIVE_ACTIVE), "active byte");
    zassert_false(hop_policy_keepalive_is_active(ESB_KEEPALIVE_IDLE), "idle byte");
}

ZTEST(hop_policy, test_loss_penalty) {
    const int8_t floor = -85;
    zassert_equal(hop_policy_loss_penalty(-40, floor), 0, "strong: no penalty");
    zassert_equal(hop_policy_loss_penalty(-85, floor), 0, "at floor: no penalty");
    zassert_equal(hop_policy_loss_penalty(-90, floor), 1, "5 dB below: 1");
    zassert_equal(hop_policy_loss_penalty(-97, floor), 3, "12 dB below: 3");
    zassert_equal(hop_policy_loss_penalty(-120, floor), HOP_POLICY_MAX_LOSS_PENALTY,
                  "deep fade: capped");
}

ZTEST(hop_policy, test_should_beacon) {
    uint8_t beaconed = 0;
    uint8_t repeats = 0;

    zassert_false(hop_policy_should_beacon(0, &beaconed, &repeats, 4), "unchanged idle epoch");

    zassert_true(hop_policy_should_beacon(1, &beaconed, &repeats, 4), "change announces");
    zassert_true(hop_policy_should_beacon(1, &beaconed, &repeats, 4), "repeat 2");
    zassert_true(hop_policy_should_beacon(1, &beaconed, &repeats, 4), "repeat 3");
    zassert_true(hop_policy_should_beacon(1, &beaconed, &repeats, 4), "repeat 4");
    zassert_false(hop_policy_should_beacon(1, &beaconed, &repeats, 4), "repeats exhausted");
    zassert_false(hop_policy_should_beacon(1, &beaconed, &repeats, 4), "stays quiet");

    zassert_true(hop_policy_should_beacon(2, &beaconed, &repeats, 4), "new change re-arms");
}
