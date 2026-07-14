// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <assert.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include "esb_keepalive.h"

void esb_keepalive_encode(uint8_t *out, size_t out_size, uint8_t state,
                          const uint8_t *position_bitmap, uint8_t battery_level,
                          const int64_t *sensor_totals_udeg, uint8_t sensor_count) {
    assert(out != NULL);
    assert(out_size >= (size_t)ESB_KEEPALIVE_LENGTH(sensor_count));
    assert(position_bitmap != NULL);
    assert(sensor_count == 0 || sensor_totals_udeg != NULL);
    out[ESB_KEEPALIVE_TAG_OFFSET] = ESB_KEEPALIVE_TAG;
    out[ESB_KEEPALIVE_STATE_OFFSET] = state;
    memcpy(&out[ESB_KEEPALIVE_BITMAP_OFFSET], position_bitmap, ESB_KEEPALIVE_BITMAP_BYTES);
    out[ESB_KEEPALIVE_BATTERY_OFFSET] = battery_level;
    for (uint8_t sensor_index = 0; sensor_index < sensor_count; sensor_index++) {
        sys_put_le64((uint64_t)sensor_totals_udeg[sensor_index],
                     &out[ESB_KEEPALIVE_SENSOR_OFFSET +
                          (size_t)sensor_index * ESB_KEEPALIVE_SENSOR_BYTES]);
    }
}

bool esb_keepalive_matches(const uint8_t *data, uint8_t length) {
    assert(data != NULL);
    if (length < ESB_KEEPALIVE_BASE_LENGTH ||
        ((length - ESB_KEEPALIVE_BASE_LENGTH) % ESB_KEEPALIVE_SENSOR_BYTES) != 0) {
        return false;
    }
    return data[ESB_KEEPALIVE_TAG_OFFSET] == ESB_KEEPALIVE_TAG;
}

uint8_t esb_keepalive_sensor_count(uint8_t length) {
    if (length < ESB_KEEPALIVE_BASE_LENGTH) {
        return 0;
    }
    return (uint8_t)((length - ESB_KEEPALIVE_BASE_LENGTH) / ESB_KEEPALIVE_SENSOR_BYTES);
}

int64_t esb_keepalive_sensor_total_udeg(const uint8_t *data, uint8_t sensor_index) {
    assert(data != NULL);
    return (int64_t)sys_get_le64(
        &data[ESB_KEEPALIVE_SENSOR_OFFSET + (size_t)sensor_index * ESB_KEEPALIVE_SENSOR_BYTES]);
}

uint8_t esb_keepalive_state(const uint8_t *data) {
    assert(data != NULL);
    return data[ESB_KEEPALIVE_STATE_OFFSET];
}

const uint8_t *esb_keepalive_bitmap(const uint8_t *data) {
    assert(data != NULL);
    return &data[ESB_KEEPALIVE_BITMAP_OFFSET];
}

uint8_t esb_keepalive_battery_level(const uint8_t *data) {
    assert(data != NULL);
    return data[ESB_KEEPALIVE_BATTERY_OFFSET];
}

void esb_keepalive_bitmap_set(uint8_t *bitmap, uint32_t position, bool pressed) {
    assert(bitmap != NULL);
    if (position >= ESB_KEEPALIVE_POSITION_COUNT) {
        return;
    }
    uint8_t *byte = &bitmap[position / 8];
    uint8_t mask = (uint8_t)(1U << (position % 8));
    if (pressed) {
        *byte = (uint8_t)(*byte | mask);
    } else {
        *byte = (uint8_t)(*byte & (uint8_t)~mask);
    }
}

bool esb_keepalive_bitmap_get(const uint8_t *bitmap, uint32_t position) {
    assert(bitmap != NULL);
    if (position >= ESB_KEEPALIVE_POSITION_COUNT) {
        return false;
    }
    return (bitmap[position / 8] & (1U << (position % 8))) != 0;
}

enum esb_keepalive_key_verdict esb_keepalive_key_verdict(const uint8_t *tracked_bitmap,
                                                         uint32_t position, bool pressed) {
    assert(tracked_bitmap != NULL);
    bool tracked = esb_keepalive_bitmap_get(tracked_bitmap, position);
    if (!pressed && !tracked) {
        return ESB_KEEPALIVE_KEY_DROP_ORPHAN_RELEASE;
    }
    if (pressed && tracked) {
        return ESB_KEEPALIVE_KEY_HEAL_LOST_RELEASE;
    }
    return ESB_KEEPALIVE_KEY_FORWARD;
}

uint32_t esb_keepalive_bitmap_diff_next(const uint8_t *tracked, const uint8_t *received,
                                        uint32_t position) {
    assert(tracked != NULL);
    assert(received != NULL);
    for (; position < ESB_KEEPALIVE_POSITION_COUNT; position++) {
        if (esb_keepalive_bitmap_get(tracked, position) !=
            esb_keepalive_bitmap_get(received, position)) {
            return position;
        }
    }
    return ESB_KEEPALIVE_POSITION_COUNT;
}
