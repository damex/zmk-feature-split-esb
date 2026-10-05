// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Uplink keepalive codec and key reconcile verdicts. */

#include <string.h>

#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>

#include "esb_keepalive.h"

#define HELD_REG_OFFSET 0
#define HELD_CODE_OFFSET 1

static size_t sensor_offset(uint8_t held_count) {
    return ESB_KEEPALIVE_HELD_OFFSET + (size_t)held_count * ESB_KEEPALIVE_HELD_BYTES;
}

static void encode_held_keys(uint8_t *out, const struct esb_keepalive_held_key *held_keys,
                             uint8_t held_count) {
    out[ESB_KEEPALIVE_HELD_COUNT_OFFSET] = held_count;
    for (uint8_t index = 0; index < held_count; index++) {
        uint8_t *entry = &out[ESB_KEEPALIVE_HELD_OFFSET + (size_t)index * ESB_KEEPALIVE_HELD_BYTES];
        entry[HELD_REG_OFFSET] = held_keys[index].reg;
        sys_put_le16(held_keys[index].code, &entry[HELD_CODE_OFFSET]);
    }
}

size_t esb_keepalive_encode(uint8_t *out, size_t out_size,
                            const struct esb_keepalive_snapshot *snapshot) {
    __ASSERT_NO_MSG(out != NULL);
    __ASSERT_NO_MSG(snapshot != NULL);
    __ASSERT_NO_MSG(snapshot->position_bitmap != NULL);
    __ASSERT_NO_MSG(snapshot->held_count == 0 || snapshot->held_keys != NULL);
    __ASSERT_NO_MSG(snapshot->sensor_count == 0 || snapshot->sensor_totals_udeg != NULL);
    size_t length = (size_t)ESB_KEEPALIVE_LENGTH(snapshot->held_count, snapshot->sensor_count);
    if (out_size < length) {
        return 0;
    }
    out[ESB_KEEPALIVE_TAG_OFFSET] = ESB_KEEPALIVE_TAG;
    out[ESB_KEEPALIVE_STATE_OFFSET] = snapshot->state;
    memcpy(&out[ESB_KEEPALIVE_BITMAP_OFFSET], snapshot->position_bitmap, ESB_KEEPALIVE_BITMAP_BYTES);
    out[ESB_KEEPALIVE_BATTERY_OFFSET] = snapshot->battery_level;
    out[ESB_KEEPALIVE_LINK_COST_OFFSET] = snapshot->link_cost_x10;
    encode_held_keys(out, snapshot->held_keys, snapshot->held_count);
    size_t totals_offset = sensor_offset(snapshot->held_count);
    for (uint8_t sensor_index = 0; sensor_index < snapshot->sensor_count; sensor_index++) {
        sys_put_le64((uint64_t)snapshot->sensor_totals_udeg[sensor_index],
                     &out[totals_offset + (size_t)sensor_index * ESB_KEEPALIVE_SENSOR_BYTES]);
    }
    return length;
}

bool esb_keepalive_matches(const uint8_t *data, uint8_t length) {
    __ASSERT_NO_MSG(data != NULL);
    if (length < ESB_KEEPALIVE_BASE_LENGTH) {
        return false;
    }
    if (data[ESB_KEEPALIVE_TAG_OFFSET] != ESB_KEEPALIVE_TAG) {
        return false;
    }
    size_t totals_offset = sensor_offset(esb_keepalive_held_count(data));
    if (length < totals_offset) {
        return false;
    }
    return ((length - totals_offset) % ESB_KEEPALIVE_SENSOR_BYTES) == 0;
}

uint8_t esb_keepalive_held_count(const uint8_t *data) {
    __ASSERT_NO_MSG(data != NULL);
    return data[ESB_KEEPALIVE_HELD_COUNT_OFFSET];
}

struct esb_keepalive_held_key esb_keepalive_held_key_at(const uint8_t *data, uint8_t index) {
    __ASSERT_NO_MSG(data != NULL);
    const uint8_t *entry = &data[ESB_KEEPALIVE_HELD_OFFSET + (size_t)index * ESB_KEEPALIVE_HELD_BYTES];
    return (struct esb_keepalive_held_key){
        .reg = entry[HELD_REG_OFFSET],
        .code = sys_get_le16(&entry[HELD_CODE_OFFSET]),
    };
}

uint8_t esb_keepalive_sensor_count(const uint8_t *data, uint8_t length) {
    __ASSERT_NO_MSG(data != NULL);
    size_t totals_offset = sensor_offset(esb_keepalive_held_count(data));
    if (length < totals_offset) {
        return 0;
    }
    return (uint8_t)((length - totals_offset) / ESB_KEEPALIVE_SENSOR_BYTES);
}

int64_t esb_keepalive_sensor_total_udeg(const uint8_t *data, uint8_t sensor_index) {
    __ASSERT_NO_MSG(data != NULL);
    size_t totals_offset = sensor_offset(esb_keepalive_held_count(data));
    return (int64_t)sys_get_le64(
        &data[totals_offset + (size_t)sensor_index * ESB_KEEPALIVE_SENSOR_BYTES]);
}

uint8_t esb_keepalive_state(const uint8_t *data) {
    __ASSERT_NO_MSG(data != NULL);
    return data[ESB_KEEPALIVE_STATE_OFFSET];
}

uint8_t esb_keepalive_peripheral_state(bool active, bool searching) {
    if (active && searching) {
        return ESB_KEEPALIVE_ACTIVE;
    }
    return ESB_KEEPALIVE_IDLE;
}

const uint8_t *esb_keepalive_bitmap(const uint8_t *data) {
    __ASSERT_NO_MSG(data != NULL);
    return &data[ESB_KEEPALIVE_BITMAP_OFFSET];
}

uint8_t esb_keepalive_battery_level(const uint8_t *data) {
    __ASSERT_NO_MSG(data != NULL);
    return data[ESB_KEEPALIVE_BATTERY_OFFSET];
}

uint8_t esb_keepalive_link_cost_x10(const uint8_t *data) {
    __ASSERT_NO_MSG(data != NULL);
    return data[ESB_KEEPALIVE_LINK_COST_OFFSET];
}

void esb_keepalive_bitmap_set(uint8_t *bitmap, uint32_t position, bool pressed) {
    __ASSERT_NO_MSG(bitmap != NULL);
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
    __ASSERT_NO_MSG(bitmap != NULL);
    if (position >= ESB_KEEPALIVE_POSITION_COUNT) {
        return false;
    }
    return (bitmap[position / 8] & (1U << (position % 8))) != 0;
}

enum esb_keepalive_key_verdict esb_keepalive_key_verdict(const uint8_t *tracked_bitmap,
                                                         uint32_t position, bool pressed) {
    __ASSERT_NO_MSG(tracked_bitmap != NULL);
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
    __ASSERT_NO_MSG(tracked != NULL);
    __ASSERT_NO_MSG(received != NULL);
    for (; position < ESB_KEEPALIVE_POSITION_COUNT; position++) {
        if (esb_keepalive_bitmap_get(tracked, position) !=
            esb_keepalive_bitmap_get(received, position)) {
            return position;
        }
    }
    return ESB_KEEPALIVE_POSITION_COUNT;
}
