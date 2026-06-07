// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hop_policy.h"

bool hop_policy_should_hop(uint8_t *bad_windows, bool window_failed, uint16_t threshold) {
    assert(bad_windows != NULL);
    if (window_failed) {
        (*bad_windows)++;
    } else {
        *bad_windows = 0;
    }
    if (*bad_windows >= threshold) {
        *bad_windows = 0;
        return true;
    }
    return false;
}

bool hop_policy_is_keepalive(uint8_t length) {
    return length == ESB_KEEPALIVE_LENGTH;
}

uint8_t hop_policy_index_next(uint8_t index, size_t count) {
    assert(count > 0);
    return (uint8_t)(((size_t)index + 1U) % count);
}

uint8_t hop_policy_channel_for_epoch(uint16_t epoch, size_t hop_count) {
    assert(hop_count > 0);
    return (uint8_t)(epoch % hop_count);
}

bool hop_policy_hop_vote(const uint8_t *link_loss, const uint8_t *weights, size_t count,
                         uint16_t threshold) {
    assert(link_loss != NULL);
    assert(weights != NULL);
    uint32_t weighted = 0;
    for (size_t index = 0; index < count; index++) {
        weighted += (uint32_t)link_loss[index] * weights[index];
    }
    return weighted >= threshold;
}

void hop_policy_accrue_loss(uint8_t *link_loss, size_t count, uint32_t heard_mask) {
    assert(link_loss != NULL);
    for (size_t index = 0; index < count; index++) {
        if (heard_mask & (1u << index)) {
            link_loss[index] = 0;
        } else if (link_loss[index] < UINT8_MAX) {
            link_loss[index]++;
        }
    }
}

bool hop_policy_central_should_hop(uint32_t heard_mask, bool ever_connected,
                                   const uint8_t *link_loss, const uint8_t *weights,
                                   size_t count, uint16_t threshold) {
    if (heard_mask == 0 || !ever_connected) {
        return false;
    }
    return hop_policy_hop_vote(link_loss, weights, count, threshold);
}

bool hop_policy_should_beacon(uint8_t epoch, uint8_t *announced_epoch, uint8_t *repeats_left,
                              uint8_t repeat_windows) {
    assert(announced_epoch != NULL);
    assert(repeats_left != NULL);
    if (epoch != *announced_epoch) {
        *announced_epoch = epoch;
        *repeats_left = repeat_windows;
    }
    if (*repeats_left > 0) {
        (*repeats_left)--;
        return true;
    }
    return false;
}
