// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Shared channel layer between hop.c and the per-role engines (hop_central.c,
 * hop_peripheral.c). The includer must define DT_DRV_COMPAT zmk_split_esb first.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>

#define HOP_COUNT DT_INST_PROP_LEN(0, hop_channels)
#define ESB_HOP_MASK_BYTES (((size_t)HOP_COUNT + 7) / 8)

/* Tag-routed, not length-routed: a mask-update length can equal the beacon's. */
#define ESB_MASK_UPDATE_TAG 0xFD
struct esb_mask_update {
    uint8_t tag;
    uint8_t version;
    uint8_t mask[ESB_HOP_MASK_BYTES];
} __attribute__((packed));
#define ESB_MASK_UPDATE_LENGTH (2 + ESB_HOP_MASK_BYTES)

static inline bool esb_is_mask_update(const uint8_t *data, uint8_t length) {
    return length == ESB_MASK_UPDATE_LENGTH && data[0] == ESB_MASK_UPDATE_TAG;
}

extern uint8_t hop_index;

void apply_hop_channel(void);

uint8_t hop_channel_at(uint8_t index);
