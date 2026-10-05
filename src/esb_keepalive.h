// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Uplink keepalive: periodic peripheral state snapshot the central reconciles against.
 * Wire: tag, hop-state byte, pressed-position bitmap, battery level, uplink
 * link cost, held input key count, held input keys, cumulative sensor totals.
 * Held input key: input-split reg, then the input code little-endian.
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
#define ESB_KEEPALIVE_IDLE 0x00
#define ESB_KEEPALIVE_ACTIVE 0x01
#define ESB_KEEPALIVE_BITMAP_OFFSET 2
#define ESB_KEEPALIVE_BITMAP_BYTES 8
#define ESB_KEEPALIVE_POSITION_COUNT (ESB_KEEPALIVE_BITMAP_BYTES * 8)
#define ESB_KEEPALIVE_BATTERY_OFFSET (ESB_KEEPALIVE_BITMAP_OFFSET + ESB_KEEPALIVE_BITMAP_BYTES)
#define ESB_KEEPALIVE_BATTERY_UNKNOWN 0xFF
#define ESB_KEEPALIVE_LINK_COST_OFFSET (ESB_KEEPALIVE_BATTERY_OFFSET + 1)
#define ESB_KEEPALIVE_HELD_COUNT_OFFSET (ESB_KEEPALIVE_LINK_COST_OFFSET + 1)
#define ESB_KEEPALIVE_HELD_OFFSET (ESB_KEEPALIVE_HELD_COUNT_OFFSET + 1)
#define ESB_KEEPALIVE_HELD_BYTES 3
#define ESB_KEEPALIVE_SENSOR_BYTES 8
#define ESB_KEEPALIVE_BASE_LENGTH ESB_KEEPALIVE_HELD_OFFSET
#define ESB_KEEPALIVE_LENGTH(held_count, sensor_count)                                             \
    (ESB_KEEPALIVE_BASE_LENGTH + (held_count) * ESB_KEEPALIVE_HELD_BYTES +                         \
     (sensor_count) * ESB_KEEPALIVE_SENSOR_BYTES)

struct esb_keepalive_held_key {
    uint8_t reg;
    uint16_t code;
};

struct esb_keepalive_snapshot {
    /* Link */
    uint8_t state;
    uint8_t link_cost_x10;
    uint8_t battery_level;

    /* Keys */
    const uint8_t *position_bitmap;
    const struct esb_keepalive_held_key *held_keys;
    uint8_t held_count;

    /* Sensors */
    const int64_t *sensor_totals_udeg;
    uint8_t sensor_count;
};

/* Returns the encoded length, 0 when out_size is too small. */
size_t esb_keepalive_encode(uint8_t *out, size_t out_size,
                            const struct esb_keepalive_snapshot *snapshot);

bool esb_keepalive_matches(const uint8_t *data, uint8_t length);

uint8_t esb_keepalive_held_count(const uint8_t *data);

/* index bounded by esb_keepalive_held_count. */
struct esb_keepalive_held_key esb_keepalive_held_key_at(const uint8_t *data, uint8_t index);

uint8_t esb_keepalive_sensor_count(const uint8_t *data, uint8_t length);

/* sensor_index bounded by esb_keepalive_sensor_count. */
int64_t esb_keepalive_sensor_total_udeg(const uint8_t *data, uint8_t sensor_index);

uint8_t esb_keepalive_state(const uint8_t *data);

uint8_t esb_keepalive_peripheral_state(bool active, bool searching);

const uint8_t *esb_keepalive_bitmap(const uint8_t *data);

uint8_t esb_keepalive_battery_level(const uint8_t *data);

uint8_t esb_keepalive_link_cost_x10(const uint8_t *data);

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
