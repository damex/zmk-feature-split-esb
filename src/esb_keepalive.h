// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Uplink keepalive: periodic peripheral state snapshot the central reconciles against.
 * Wire: tag, hop-state byte, pressed-position bitmap, battery level, cumulative
 * sensor totals.
 * Tag 0xFF cannot collide with event packets, whose first byte is an event type.
 * Positions above ESB_KEEPALIVE_POSITION_COUNT are not covered.
 * Battery level is ESB_KEEPALIVE_BATTERY_UNKNOWN when the peripheral does not report it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESB_KEEPALIVE_TAG 0xFF
#define ESB_KEEPALIVE_TAG_OFFSET 0
#define ESB_KEEPALIVE_STATE_OFFSET 1
#define ESB_KEEPALIVE_BITMAP_OFFSET 2
#define ESB_KEEPALIVE_BITMAP_BYTES 8
#define ESB_KEEPALIVE_POSITION_COUNT (ESB_KEEPALIVE_BITMAP_BYTES * 8)
#define ESB_KEEPALIVE_BATTERY_OFFSET (ESB_KEEPALIVE_BITMAP_OFFSET + ESB_KEEPALIVE_BITMAP_BYTES)
#define ESB_KEEPALIVE_BATTERY_UNKNOWN 0xFF
#define ESB_KEEPALIVE_SENSOR_OFFSET (ESB_KEEPALIVE_BATTERY_OFFSET + 1)
#define ESB_KEEPALIVE_SENSOR_BYTES 8
#define ESB_KEEPALIVE_BASE_LENGTH ESB_KEEPALIVE_SENSOR_OFFSET
#define ESB_KEEPALIVE_LENGTH(sensor_count)                                                         \
    (ESB_KEEPALIVE_BASE_LENGTH + (sensor_count) * ESB_KEEPALIVE_SENSOR_BYTES)

void esb_keepalive_encode(uint8_t *out, size_t out_size, uint8_t state,
                          const uint8_t *position_bitmap, uint8_t battery_level,
                          const int64_t *sensor_totals_udeg, uint8_t sensor_count);

bool esb_keepalive_matches(const uint8_t *data, uint8_t length);

uint8_t esb_keepalive_sensor_count(uint8_t length);

/* sensor_index bounded by esb_keepalive_sensor_count. */
int64_t esb_keepalive_sensor_total_udeg(const uint8_t *data, uint8_t sensor_index);

uint8_t esb_keepalive_state(const uint8_t *data);

const uint8_t *esb_keepalive_bitmap(const uint8_t *data);

uint8_t esb_keepalive_battery_level(const uint8_t *data);

/* Out-of-range positions: set is ignored, get reads false. */
void esb_keepalive_bitmap_set(uint8_t *bitmap, uint32_t position, bool pressed);
bool esb_keepalive_bitmap_get(const uint8_t *bitmap, uint32_t position);

/* Live-stream healing: orphan release (lost press) drops, repeated press (lost
 * release) heals the missing release before forwarding.
 * Caller bounds position: beyond the bitmap there is nothing to classify. */
enum esb_keepalive_key_verdict {
    ESB_KEEPALIVE_KEY_FORWARD,
    ESB_KEEPALIVE_KEY_DROP_ORPHAN_RELEASE,
    ESB_KEEPALIVE_KEY_HEAL_LOST_RELEASE,
};
enum esb_keepalive_key_verdict esb_keepalive_key_verdict(const uint8_t *tracked_bitmap,
                                                         uint32_t position, bool pressed);

/* Next position whose bit differs between the bitmaps, scanning from position
 * upward. ESB_KEEPALIVE_POSITION_COUNT when none differ. */
uint32_t esb_keepalive_bitmap_diff_next(const uint8_t *tracked, const uint8_t *received,
                                        uint32_t position);
