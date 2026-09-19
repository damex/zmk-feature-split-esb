// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <zephyr/ztest.h>

#include "spsc_latch.h"

ZTEST_SUITE(spsc_latch, NULL, NULL, NULL, NULL, NULL);

ZTEST(spsc_latch, test_empty_peek_is_null) {
    struct spsc_latch latch = {.slot_count = 2};
    zassert_is_null(spsc_latch_peek(&latch), "empty latch peeks NULL");
}

ZTEST(spsc_latch, test_claim_round_robin) {
    struct spsc_latch latch = {.slot_count = 2};
    zassert_equal(spsc_latch_claim(&latch), 0, "first claim");
    zassert_equal(spsc_latch_claim(&latch), 1, "second claim");
    zassert_equal(spsc_latch_claim(&latch), 0, "third wraps");
    zassert_equal(spsc_latch_claim(&latch), 1, "fourth wraps");
}

ZTEST(spsc_latch, test_claim_larger_pool) {
    struct spsc_latch latch = {.slot_count = 4};
    zassert_equal(spsc_latch_claim(&latch), 0, NULL);
    zassert_equal(spsc_latch_claim(&latch), 1, NULL);
    zassert_equal(spsc_latch_claim(&latch), 2, NULL);
    zassert_equal(spsc_latch_claim(&latch), 3, NULL);
    zassert_equal(spsc_latch_claim(&latch), 0, "wraps at slot_count");
}

ZTEST(spsc_latch, test_publish_then_peek) {
    struct spsc_latch latch = {.slot_count = 2};
    int pool[2] = {0};
    spsc_latch_publish(&latch, &pool[0]);
    zassert_equal_ptr(spsc_latch_peek(&latch), &pool[0], "peek returns published");
}

ZTEST(spsc_latch, test_publish_overwrites_unread) {
    struct spsc_latch latch = {.slot_count = 2};
    int pool[2] = {0};
    spsc_latch_publish(&latch, &pool[0]);
    spsc_latch_publish(&latch, &pool[1]);
    zassert_equal_ptr(spsc_latch_peek(&latch), &pool[1], "latest wins");
}

ZTEST(spsc_latch, test_release_matching_clears) {
    struct spsc_latch latch = {.slot_count = 2};
    int pool[2] = {0};
    spsc_latch_publish(&latch, &pool[0]);
    zassert_true(spsc_latch_release(&latch, &pool[0]), "matching release succeeds");
    zassert_is_null(spsc_latch_peek(&latch), "cleared after release");
}

ZTEST(spsc_latch, test_release_stale_leaves_newer_publish) {
    struct spsc_latch latch = {.slot_count = 2};
    int pool[2] = {0};
    spsc_latch_publish(&latch, &pool[0]);
    spsc_latch_publish(&latch, &pool[1]);
    zassert_false(spsc_latch_release(&latch, &pool[0]), "stale release refused");
    zassert_equal_ptr(spsc_latch_peek(&latch), &pool[1], "newer publish intact");
}

ZTEST(spsc_latch, test_release_empty_returns_false) {
    struct spsc_latch latch = {.slot_count = 2};
    int marker = 0;
    zassert_false(spsc_latch_release(&latch, &marker), "release on empty is false");
}
