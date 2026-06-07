// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Length-1 acked keepalive: a retransmit can't replay stale deltas, and length 1 marks
 * it as a keepalive rather than a split message. */
#define ESB_KEEPALIVE_LENGTH 1

/* Returns true and clears the streak after threshold consecutive failures. */
bool hop_policy_should_hop(uint8_t *bad_windows, bool window_failed, uint16_t threshold);

bool hop_policy_is_keepalive(uint8_t length);

uint8_t hop_policy_index_next(uint8_t index, size_t count);

/* Both ends derive the same channel index from the central's epoch. */
uint8_t hop_policy_channel_for_epoch(uint16_t epoch, size_t hop_count);

/* Central hop decision: weighted sum of per-peripheral link loss, true at threshold. */
bool hop_policy_hop_vote(const uint8_t *link_loss, const uint8_t *weights, size_t count,
                         uint16_t threshold);

/* Per window, clear loss for pipes heard (their bit set in heard_mask) and increment it
 * for silent ones, saturating at UINT8_MAX. */
void hop_policy_accrue_loss(uint8_t *link_loss, size_t count, uint32_t heard_mask);

/* Central hop gate.
 * A peripheral that went silent this window (heard_mask == 0) must not drive a hop, or a
 * lone central wanders off-channel chasing a peer that has left.
 * A central that never connected stays put. Otherwise defer to the weighted loss vote. */
bool hop_policy_central_should_hop(uint32_t heard_mask, bool ever_connected,
                                   const uint8_t *link_loss, const uint8_t *weights,
                                   size_t count, uint16_t threshold);

/* Beacon scheduling: announce the epoch only while it is fresh.
 * A changed epoch arms repeat_windows announcements, then goes quiet, so a steady stream
 * never crowds commands out of the reverse channel. */
bool hop_policy_should_beacon(uint8_t epoch, uint8_t *announced_epoch, uint8_t *repeats_left,
                              uint8_t repeat_windows);
