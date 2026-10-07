// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Hop engine internals shared by both roles.
 * Includer defines DT_DRV_COMPAT zmk_split_esb first.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#define HOP_COUNT DT_INST_PROP_LEN(0, hop_channels)
#define ESB_HOP_MASK_BYTES (((size_t)HOP_COUNT + 7) / 8)

/* Tag-routed, not length-routed: a mask-update length can equal the beacon's. */
#define ESB_MASK_UPDATE_TAG 0xFD
struct esb_mask_update {
    uint8_t tag;
    uint8_t mask[ESB_HOP_MASK_BYTES];
} __attribute__((packed));
#define ESB_MASK_UPDATE_LENGTH (1 + ESB_HOP_MASK_BYTES)

static inline bool esb_is_mask_update(const uint8_t *data, uint8_t length) {
    if (length != ESB_MASK_UPDATE_LENGTH) {
        return false;
    }
    return data[offsetof(struct esb_mask_update, tag)] == ESB_MASK_UPDATE_TAG;
}

#define ESB_BEACON_TAG 0xFE
#define ESB_BEACON_PEER_COUNT DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals))

struct esb_beacon_peer {
    uint8_t battery;
    int8_t rssi_dbm;
} __attribute__((packed));

struct esb_beacon {
    uint8_t tag;
    uint8_t epoch;
    uint8_t hid_modifiers;
    uint8_t hid_indicators;
    struct esb_beacon_peer peers[ESB_BEACON_PEER_COUNT];
} __attribute__((packed));
#define ESB_BEACON_LENGTH (4 + ESB_BEACON_PEER_COUNT * 2)
BUILD_ASSERT(sizeof(struct esb_beacon) == ESB_BEACON_LENGTH, "beacon wire size");

static inline bool esb_is_beacon(const uint8_t *data, uint8_t length) {
    if (length != ESB_BEACON_LENGTH) {
        return false;
    }
    return data[offsetof(struct esb_beacon, tag)] == ESB_BEACON_TAG;
}

extern uint8_t hop_index;

/* Channels never masked.
 * hop-anchors picks them, else up to three spread over the pool. */
#define ESB_HOP_ANCHOR_COUNT DT_INST_PROP_LEN_OR(0, hop_anchors, MIN((uint8_t)3, (uint8_t)HOP_COUNT))

#define ESB_HOP_LOSS_DETECT_MS (2 * DT_INST_PROP(0, idle_keepalive_ms))

/* Dwell per channel covers one central decision tick plus the beacon it sends back. */
#define ESB_HOP_SWEEP_DWELL_WINDOWS                                                                  \
    (DIV_ROUND_UP(DT_INST_PROP(0, idle_keepalive_ms), DT_INST_PROP(0, hop_window_ms)) + 2)

void apply_hop_channel(void);

uint8_t hop_channel_at(uint8_t index);

bool hop_is_anchor_index(uint8_t index);
