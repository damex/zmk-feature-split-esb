// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Length-1 acked keepalive: a retransmit can't replay stale deltas, length 1 marks it as a
 * keepalive rather than a split message, and its one byte tells the central whether the
 * peripheral is actively polling. */
#define ESB_KEEPALIVE_LENGTH 1
#define ESB_KEEPALIVE_IDLE 0x00
#define ESB_KEEPALIVE_ACTIVE 0x01

/* True when a keepalive byte reports the peripheral is actively polling. */
bool hop_policy_keepalive_is_active(uint8_t byte);

/* Graded per-window loss for one pipe from its motion RSSI (dBm, negative).
 * Zero at or above the floor, then one point per HOP_POLICY_RSSI_GRADE_STEP_DB below it,
 * capped at HOP_POLICY_MAX_LOSS_PENALTY, so a weaker signal reaches the hop threshold sooner. */
#define HOP_POLICY_RSSI_GRADE_STEP_DB 6
#define HOP_POLICY_MAX_LOSS_PENALTY 4
uint8_t hop_policy_loss_penalty(int8_t rssi_dbm, int8_t floor_dbm);

/* Returns true and clears the streak after threshold consecutive failures. */
bool hop_policy_should_hop(uint8_t *bad_windows, bool window_failed, uint16_t threshold);

bool hop_policy_is_keepalive(uint8_t length);

uint8_t hop_policy_index_next(uint8_t index, size_t count);

/* Both ends derive the same channel index from the central's epoch. */
uint8_t hop_policy_channel_for_epoch(uint16_t epoch, size_t hop_count);

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
bool hop_policy_should_beacon(uint8_t epoch, uint8_t *announced_epoch, uint8_t *repeats_left,
                              uint8_t repeat_windows);
