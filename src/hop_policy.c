// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hop_policy.h"

uint8_t hop_policy_saturating_add(uint8_t value, uint8_t add) {
    if (value > UINT8_MAX - add) {
        return UINT8_MAX;
    }
    return (uint8_t)(value + add);
}

bool hop_policy_should_hop(uint8_t *bad_windows, uint8_t penalty, uint16_t threshold) {
    assert(bad_windows != NULL);
    if (penalty == 0) {
        *bad_windows = 0;
        return false;
    }
    *bad_windows = hop_policy_saturating_add(*bad_windows, penalty);
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

uint16_t hop_policy_ewma_update(uint16_t ewma_x10, uint8_t sample) {
    int32_t sample_x10 = (int32_t)sample * 10;
    int32_t delta = (sample_x10 - (int32_t)ewma_x10) / (1 << HOP_POLICY_ATTEMPTS_EWMA_SHIFT);
    return (uint16_t)((int32_t)ewma_x10 + delta);
}

uint8_t hop_policy_adaptive_retransmits(uint16_t ewma_x10, uint8_t count_min, uint8_t count_max) {
    assert(count_min <= count_max);
    if (ewma_x10 <= HOP_POLICY_RETRY_EWMA_LOW_X10) {
        return count_min;
    }
    if (ewma_x10 >= HOP_POLICY_RETRY_EWMA_HIGH_X10) {
        return count_max;
    }
    uint32_t span = HOP_POLICY_RETRY_EWMA_HIGH_X10 - HOP_POLICY_RETRY_EWMA_LOW_X10;
    uint32_t into = (uint32_t)ewma_x10 - HOP_POLICY_RETRY_EWMA_LOW_X10;
    return (uint8_t)(count_min + (into * (uint32_t)(count_max - count_min)) / span);
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

void hop_policy_camp_step(uint8_t *camp_anchor, uint16_t *camp_dwell, uint8_t anchor_count,
                          uint16_t dwell_reload) {
    assert(camp_anchor != NULL);
    assert(camp_dwell != NULL);
    if (*camp_dwell > 0) {
        (*camp_dwell)--;
        return;
    }
    *camp_anchor = hop_policy_index_next(*camp_anchor, anchor_count);
    *camp_dwell = dwell_reload;
}

uint8_t hop_policy_channel_for_epoch(uint16_t epoch, size_t hop_count) {
    assert(hop_count > 0);
    return (uint8_t)(epoch % hop_count);
}

uint8_t hop_policy_anchor_default_index(size_t slot, size_t pool_count, size_t anchor_count) {
    assert(anchor_count > 0);
    assert(anchor_count <= pool_count);
    assert(slot < anchor_count);
    return (uint8_t)((pool_count * (2 * slot + 1)) / (2 * anchor_count));
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

static uint8_t moving_pipe_penalty(int8_t rssi_dbm, uint8_t link_cost_x10, int8_t floor_dbm) {
    uint8_t rssi_penalty = hop_policy_loss_penalty(rssi_dbm, floor_dbm);
    uint8_t cost_penalty = hop_policy_link_cost_penalty(link_cost_x10);
    return (rssi_penalty > cost_penalty) ? rssi_penalty : cost_penalty;
}

uint8_t hop_policy_link_cost_penalty(uint8_t cost_x10) {
    if (cost_x10 <= HOP_POLICY_RETRY_EWMA_LOW_X10) {
        return 0;
    }
    int penalty = 1 + ((int)cost_x10 - HOP_POLICY_RETRY_EWMA_LOW_X10) /
                          HOP_POLICY_LINK_COST_GRADE_STEP;
    if (penalty > HOP_POLICY_MAX_LOSS_PENALTY) {
        penalty = HOP_POLICY_MAX_LOSS_PENALTY;
    }
    return (uint8_t)penalty;
}

uint8_t hop_policy_window_penalty(uint32_t motion_mask, uint32_t active_mask,
                                  const int8_t *rssi_dbm, const uint8_t *link_cost_x10,
                                  int8_t floor_dbm, size_t count) {
    assert(rssi_dbm != NULL);
    assert(link_cost_x10 != NULL);
    uint8_t worst = 0;
    for (size_t index = 0; index < count; index++) {
        if (!(active_mask & (1u << index))) {
            continue;
        }
        uint8_t penalty;
        if (motion_mask & (1u << index)) {
            penalty = moving_pipe_penalty(rssi_dbm[index], link_cost_x10[index], floor_dbm);
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

void hop_policy_score_update(uint8_t *score, uint8_t penalty, uint8_t decay) {
    assert(score != NULL);
    if (penalty == 0) {
        *score = (*score > decay) ? (uint8_t)(*score - decay) : 0;
        return;
    }
    *score = hop_policy_saturating_add(*score, penalty);
}

size_t hop_policy_worst_channel(const uint8_t *channel_bad, const uint8_t *mask,
                                const uint8_t *anchor_mask, size_t pool_count,
                                uint16_t mask_threshold) {
    assert(channel_bad != NULL);
    assert(mask != NULL);
    assert(anchor_mask != NULL);
    size_t worst = pool_count;
    for (size_t channel = 0; channel < pool_count; channel++) {
        if (hop_policy_mask_get(anchor_mask, channel) || !hop_policy_mask_get(mask, channel)) {
            continue;
        }
        if (channel_bad[channel] < mask_threshold) {
            continue;
        }
        if (worst == pool_count || channel_bad[channel] > channel_bad[worst]) {
            worst = channel;
        }
    }
    return worst;
}

uint16_t hop_policy_retest_threshold(uint16_t base_windows, uint8_t level) {
    if (level > HOP_POLICY_RETEST_LEVEL_MAX) {
        level = HOP_POLICY_RETEST_LEVEL_MAX;
    }
    uint32_t threshold = (uint32_t)base_windows << level;
    if (threshold > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)threshold;
}

uint8_t hop_policy_index_next_active(uint8_t index, const uint8_t *mask, size_t count) {
    assert(mask != NULL);
    assert(count > 0);
    for (size_t step = 1; step <= count; step++) {
        size_t candidate = ((size_t)index + step) % count;
        if (hop_policy_mask_get(mask, candidate)) {
            return (uint8_t)candidate;
        }
    }
    return index;
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
                            uint32_t active_mask, const int8_t *rssi_dbm,
                            const uint8_t *link_cost_x10, int8_t floor_dbm) {
    assert(link_loss != NULL);
    assert(rssi_dbm != NULL);
    assert(link_cost_x10 != NULL);
    for (size_t index = 0; index < count; index++) {
        if (!(active_mask & (1u << index))) {
            link_loss[index] = 0;
            continue;
        }
        uint8_t penalty;
        if (motion_mask & (1u << index)) {
            penalty = moving_pipe_penalty(rssi_dbm[index], link_cost_x10[index], floor_dbm);
        } else {
            penalty = HOP_POLICY_MAX_LOSS_PENALTY;
        }
        if (penalty == 0) {
            link_loss[index] = 0;
        } else {
            link_loss[index] = hop_policy_saturating_add(link_loss[index], penalty);
        }
    }
}

size_t hop_policy_survey_mask(const int8_t *energy_dbm, size_t pool_count,
                              const uint8_t *anchor_mask, size_t min_active,
                              int8_t threshold_dbm, uint8_t *mask) {
    assert(energy_dbm != NULL);
    assert(anchor_mask != NULL);
    assert(mask != NULL);
    size_t masked = 0;
    while (hop_policy_mask_active_count(mask, pool_count) > min_active) {
        size_t worst = pool_count;
        for (size_t channel = 0; channel < pool_count; channel++) {
            if (hop_policy_mask_get(anchor_mask, channel) ||
                !hop_policy_mask_get(mask, channel)) {
                continue;
            }
            if (energy_dbm[channel] < threshold_dbm) {
                continue;
            }
            if (worst == pool_count || energy_dbm[channel] > energy_dbm[worst]) {
                worst = channel;
            }
        }
        if (worst == pool_count) {
            break;
        }
        hop_policy_mask_set(mask, worst, false);
        masked++;
    }
    return masked;
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
