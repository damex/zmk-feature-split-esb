// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESB_BEACON_TAG 0xFE
struct esb_beacon {
    uint8_t tag;
    uint8_t epoch;
    int8_t rssi_dbm;
    uint8_t mask_version;
} __attribute__((packed));

#define ESB_BEACON_LENGTH 4
_Static_assert(sizeof(struct esb_beacon) == ESB_BEACON_LENGTH, "beacon wire size");

/* Keepalive state byte values: whether the peripheral is actively polling. */
#define ESB_KEEPALIVE_IDLE 0x00
#define ESB_KEEPALIVE_ACTIVE 0x01

bool hop_policy_keepalive_is_active(uint8_t byte);

/* ESB RSSI is a positive magnitude, dBm is its negative. */
int8_t hop_policy_rssi_to_dbm(int8_t rssi_magnitude);

/* Graded per-window loss for one pipe from its motion RSSI (dBm, negative).
 * Zero at or above the floor, then one point per HOP_POLICY_RSSI_GRADE_STEP_DB below it,
 * capped at HOP_POLICY_MAX_LOSS_PENALTY, so a weaker signal reaches the hop threshold sooner. */
#define HOP_POLICY_RSSI_GRADE_STEP_DB 6
#define HOP_POLICY_MAX_LOSS_PENALTY 4
uint8_t hop_policy_loss_penalty(int8_t rssi_dbm, int8_t floor_dbm);

/* Accumulate a graded sweep penalty, returning true and clearing the streak once it
 * reaches threshold. A zero penalty (clean window) resets the streak. */
bool hop_policy_should_hop(uint8_t *bad_windows, uint8_t penalty, uint16_t threshold);

/* Graded sweep penalty from a transmit's retransmit count: zero up to good_attempts, then
 * one point per HOP_POLICY_TX_ATTEMPTS_GRADE_STEP over it, capped at HOP_POLICY_MAX_LOSS_PENALTY. */
#define HOP_POLICY_GOOD_TX_ATTEMPTS 2
#define HOP_POLICY_TX_ATTEMPTS_GRADE_STEP 4
uint8_t hop_policy_attempts_penalty(uint8_t attempts, uint8_t good_attempts);

bool hop_policy_is_beacon(const uint8_t *data, uint8_t length);

uint8_t hop_policy_index_next(uint8_t index, size_t count);

/* Both ends derive the same channel index from the central's epoch. */
uint8_t hop_policy_channel_for_epoch(uint16_t epoch, size_t hop_count);

/* Active-channel bitmap over the pool: bit set means the channel is in use. */
bool hop_policy_mask_get(const uint8_t *mask, size_t index);
void hop_policy_mask_set(uint8_t *mask, size_t index, bool active);
size_t hop_policy_mask_active_count(const uint8_t *mask, size_t pool_count);

/* Zero when no pipe polls this window: an untested channel is neither blamed nor cleared. */
uint8_t hop_policy_window_penalty(uint32_t motion_mask, uint32_t active_mask,
                                  const int8_t *rssi_dbm, int8_t floor_dbm, size_t count);

/* Both ends derive the same channel from (epoch, mask). */
uint8_t hop_policy_channel_for_epoch_masked(uint16_t epoch, const uint8_t *mask, size_t pool_count);

/* Central hop decision: weighted sum of per-peripheral link loss, true at threshold. */
bool hop_policy_hop_vote(const uint8_t *link_loss, const uint8_t *weights, size_t count,
                         uint16_t threshold);

/* Per window, accrue graded per-pipe loss from poll traffic, so only an actively-polling
 * pipe whose link is degrading drives a hop, and the weaker it is the sooner.
 * Motion (bit in motion_mask) adds hop_policy_loss_penalty(rssi_dbm[pipe]). An active pipe
 * with no motion is fully lost and adds HOP_POLICY_MAX_LOSS_PENALTY. A healthy (penalty 0)
 * or idle/absent pipe clears to zero. Loss saturates at UINT8_MAX. */
void hop_policy_accrue_loss(uint8_t *link_loss, size_t count, uint32_t motion_mask,
                            uint32_t active_mask, const int8_t *rssi_dbm, int8_t floor_dbm);

/* Beacon scheduling: announce the epoch only while it is fresh.
 * A changed epoch arms repeat_windows announcements, then goes quiet, so a steady stream
 * never crowds commands out of the reverse channel. */
bool hop_policy_should_beacon(uint8_t epoch, uint8_t *beaconed_epoch, uint8_t *repeats_left,
                              uint8_t repeat_windows);
