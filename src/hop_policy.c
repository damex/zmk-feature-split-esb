// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hop_policy.h"

bool hop_policy_should_hop(uint8_t *bad_windows, uint8_t penalty, uint16_t threshold) {
    assert(bad_windows != NULL);
    if (penalty == 0) {
        *bad_windows = 0;
        return false;
    }
    if (*bad_windows > UINT8_MAX - penalty) {
        *bad_windows = UINT8_MAX;
    } else {
        *bad_windows = (uint8_t)(*bad_windows + penalty);
    }
    if (*bad_windows >= threshold) {
        *bad_windows = 0;
        return true;
    }
    return false;
}

uint8_t hop_policy_attempts_penalty(uint8_t attempts, uint8_t good_attempts) {
    if (attempts <= good_attempts) {
        return 0;
    }
    int over = (int)attempts - (int)good_attempts;
    int penalty = 1 + over / HOP_POLICY_TX_ATTEMPTS_GRADE_STEP;
    if (penalty > HOP_POLICY_MAX_LOSS_PENALTY) {
        penalty = HOP_POLICY_MAX_LOSS_PENALTY;
    }
    return (uint8_t)penalty;
}

bool hop_policy_is_beacon(const uint8_t *data, uint8_t length) {
    assert(data != NULL);
    return length == ESB_BEACON_LENGTH && data[0] == ESB_BEACON_TAG;
}

bool hop_policy_keepalive_is_active(uint8_t byte) {
    return byte == ESB_KEEPALIVE_ACTIVE;
}

int8_t hop_policy_rssi_to_dbm(int8_t rssi_magnitude) {
    return (int8_t)(-rssi_magnitude);
}

uint8_t hop_policy_loss_penalty(int8_t rssi_dbm, int8_t floor_dbm) {
    if (rssi_dbm >= floor_dbm) {
        return 0;
    }
    int below = (int)floor_dbm - (int)rssi_dbm;
    int penalty = 1 + below / HOP_POLICY_RSSI_GRADE_STEP_DB;
    if (penalty > HOP_POLICY_MAX_LOSS_PENALTY) {
        penalty = HOP_POLICY_MAX_LOSS_PENALTY;
    }
    return (uint8_t)penalty;
}

uint8_t hop_policy_index_next(uint8_t index, size_t count) {
    assert(count > 0);
    return (uint8_t)(((size_t)index + 1U) % count);
}

uint8_t hop_policy_channel_for_epoch(uint16_t epoch, size_t hop_count) {
    assert(hop_count > 0);
    return (uint8_t)(epoch % hop_count);
}

bool hop_policy_mask_get(const uint8_t *mask, size_t index) {
    assert(mask != NULL);
    return (mask[index / 8] & (uint8_t)(1u << (index % 8))) != 0;
}

void hop_policy_mask_set(uint8_t *mask, size_t index, bool active) {
    assert(mask != NULL);
    uint8_t bit = (uint8_t)(1u << (index % 8));
    if (active) {
        mask[index / 8] = (uint8_t)(mask[index / 8] | bit);
    } else {
        mask[index / 8] = (uint8_t)(mask[index / 8] & (uint8_t)~bit);
    }
}

uint8_t hop_policy_window_penalty(uint32_t motion_mask, uint32_t active_mask,
                                  const int8_t *rssi_dbm, int8_t floor_dbm, size_t count) {
    assert(rssi_dbm != NULL);
    uint8_t worst = 0;
    for (size_t index = 0; index < count; index++) {
        if (!(active_mask & (1u << index))) {
            continue;
        }
        uint8_t penalty;
        if (motion_mask & (1u << index)) {
            penalty = hop_policy_loss_penalty(rssi_dbm[index], floor_dbm);
        } else {
            penalty = HOP_POLICY_MAX_LOSS_PENALTY;
        }
        if (penalty > worst) {
            worst = penalty;
        }
    }
    return worst;
}

size_t hop_policy_mask_active_count(const uint8_t *mask, size_t pool_count) {
    assert(mask != NULL);
    size_t count = 0;
    for (size_t index = 0; index < pool_count; index++) {
        if (hop_policy_mask_get(mask, index)) {
            count++;
        }
    }
    return count;
}

uint8_t hop_policy_channel_for_epoch_masked(uint16_t epoch, const uint8_t *mask, size_t pool_count) {
    assert(mask != NULL);
    assert(pool_count > 0);
    size_t base = (size_t)(epoch % pool_count);
    for (size_t step = 0; step < pool_count; step++) {
        size_t index = (base + step) % pool_count;
        if (hop_policy_mask_get(mask, index)) {
            return (uint8_t)index;
        }
    }
    return (uint8_t)base;
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

void hop_policy_accrue_loss(uint8_t *link_loss, size_t count, uint32_t motion_mask,
                            uint32_t active_mask, const int8_t *rssi_dbm, int8_t floor_dbm) {
    assert(link_loss != NULL);
    assert(rssi_dbm != NULL);
    for (size_t index = 0; index < count; index++) {
        if (!(active_mask & (1u << index))) {
            link_loss[index] = 0;
            continue;
        }
        uint8_t penalty;
        if (motion_mask & (1u << index)) {
            penalty = hop_policy_loss_penalty(rssi_dbm[index], floor_dbm);
        } else {
            penalty = HOP_POLICY_MAX_LOSS_PENALTY;
        }
        if (penalty == 0) {
            link_loss[index] = 0;
        } else if (link_loss[index] > UINT8_MAX - penalty) {
            link_loss[index] = UINT8_MAX;
        } else {
            link_loss[index] = (uint8_t)(link_loss[index] + penalty);
        }
    }
}

bool hop_policy_should_beacon(uint8_t epoch, uint8_t *beaconed_epoch, uint8_t *repeats_left,
                              uint8_t repeat_windows) {
    assert(beaconed_epoch != NULL);
    assert(repeats_left != NULL);
    if (epoch != *beaconed_epoch) {
        *beaconed_epoch = epoch;
        *repeats_left = repeat_windows;
    }
    if (*repeats_left > 0) {
        (*repeats_left)--;
        return true;
    }
    return false;
}
