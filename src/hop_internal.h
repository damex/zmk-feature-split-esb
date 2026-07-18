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
    return length == ESB_MASK_UPDATE_LENGTH && data[0] == ESB_MASK_UPDATE_TAG;
}

extern uint8_t hop_index;

/* Rendezvous channels, never masked, both roles cycle them on a lost link.
 * hop-anchors picks them, else the first three pool entries. */
#define ESB_HOP_ANCHOR_COUNT DT_INST_PROP_LEN_OR(0, hop_anchors, MIN((uint8_t)3, (uint8_t)HOP_COUNT))
#define ESB_HOP_DIP_PERIOD 3
#define ESB_HOP_DIP_ABSENT_PERIOD 6
/* A stable channel is re-found by the peripheral's sweep, so a dip here only covers
 * a sweep that never lands. */
#define ESB_HOP_DIP_STABLE_PERIOD 32
/* Outlasts the central's slowest full sweep so a dip is bound to land on the camped anchor.
 * Held near one sweep, not multiples, so a jammed anchor is abandoned quickly. */
#define ESB_HOP_ANCHOR_DWELL_WINDOWS                                                                 \
    (((ESB_HOP_ANCHOR_COUNT + 1) * ESB_HOP_DIP_ABSENT_PERIOD *                                       \
      DT_INST_PROP(0, idle_keepalive_ms)) / DT_INST_PROP(0, hop_window_ms))

#define ESB_HOP_LOSS_DETECT_MS 256

/* Dwell per channel covers one central decision tick plus the beacon it sends back. */
#define ESB_HOP_SWEEP_DWELL_WINDOWS                                                                  \
    (DIV_ROUND_UP(DT_INST_PROP(0, idle_keepalive_ms), DT_INST_PROP(0, hop_window_ms)) + 2)
#define ESB_HOP_SWEEP_WINDOWS ((HOP_COUNT + 2) * ESB_HOP_SWEEP_DWELL_WINDOWS)

void apply_hop_channel(void);

/* Retune to a given slot, leaving hop_index unchanged for a transient
 * anchor visit that returns to the live channel. */
void apply_channel_index(uint8_t index);

uint8_t hop_channel_at(uint8_t index);

uint8_t hop_anchor_index_at(uint8_t slot);
bool hop_is_anchor_index(uint8_t index);
