// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <assert.h>
#include <string.h>

#include "esb_keepalive.h"

void esb_keepalive_encode(uint8_t *out, uint8_t state, const uint8_t *position_bitmap,
                          uint8_t battery_level) {
    assert(out != NULL);
    assert(position_bitmap != NULL);
    out[ESB_KEEPALIVE_TAG_OFFSET] = ESB_KEEPALIVE_TAG;
    out[ESB_KEEPALIVE_STATE_OFFSET] = state;
    memcpy(&out[ESB_KEEPALIVE_BITMAP_OFFSET], position_bitmap, ESB_KEEPALIVE_BITMAP_BYTES);
    out[ESB_KEEPALIVE_BATTERY_OFFSET] = battery_level;
}

bool esb_keepalive_matches(const uint8_t *data, uint8_t length) {
    assert(data != NULL);
    return length == ESB_KEEPALIVE_LENGTH && data[ESB_KEEPALIVE_TAG_OFFSET] == ESB_KEEPALIVE_TAG;
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
