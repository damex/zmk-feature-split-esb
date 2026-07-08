// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Shared channel layer for the hop engine.
 * Owns the channel table and the radio retune; the per-role engines (hop_central.c,
 * hop_peripheral.c) set hop_index and call apply_hop_channel.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "hop.h"
#include "hop_internal.h"

static const uint8_t hop_channels[] = DT_INST_PROP(0, hop_channels);
BUILD_ASSERT(HOP_COUNT >= 1, "hop-channels needs at least one channel");
uint8_t hop_index;

/* Retune with the ISR masked: esb_stop_rx() briefly nulls the radio disabled-callback,
 * and a DISABLED event landing there mid-reconfigure faults the device.
 * Skip the work when the channel is unchanged, so a redundant retune never tears down RX. */
static uint8_t applied_index;
void apply_channel_index(uint8_t index) {
    if (index == applied_index) {
        return;
    }
    unsigned int key = irq_lock();
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    esb_stop_rx();
    int set_error = esb_set_rf_channel(hop_channels[index]);
    esb_start_rx();
#else
    int set_error = esb_set_rf_channel(hop_channels[index]);
#endif
    irq_unlock(key);
    if (set_error == 0) {
        applied_index = index;
    }
}

void apply_hop_channel(void) {
    apply_channel_index(hop_index);
}

uint8_t hop_current_channel(void) {
    return hop_channels[hop_index];
}

uint8_t hop_channel_at(uint8_t index) {
    return hop_channels[index];
}

BUILD_ASSERT(ESB_HOP_ANCHOR_COUNT >= 1 && ESB_HOP_ANCHOR_COUNT <= HOP_COUNT,
             "hop-anchors is empty or larger than hop-channels");

#define POOL_HAS_OR(node_id, prop, index, needle)                                                  \
    || (DT_PROP_BY_IDX(node_id, prop, index) == (needle))
#define POOL_HAS(needle) (0 DT_INST_FOREACH_PROP_ELEM_VARGS(0, hop_channels, POOL_HAS_OR, needle))
#define ANCHOR_IN_POOL(node_id, prop, index)                                                       \
    BUILD_ASSERT(POOL_HAS(DT_PROP_BY_IDX(node_id, prop, index)),                                   \
                 "hop-anchors lists a channel absent from hop-channels");
#if DT_INST_NODE_HAS_PROP(0, hop_anchors)
DT_INST_FOREACH_PROP_ELEM(0, hop_anchors, ANCHOR_IN_POOL)
#endif

static uint8_t anchor_indices[ESB_HOP_ANCHOR_COUNT];
static bool anchors_resolved;

static void resolve_anchors(void) {
    if (anchors_resolved) {
        return;
    }
#if DT_INST_NODE_HAS_PROP(0, hop_anchors)
    static const uint8_t anchor_channels[] = DT_INST_PROP(0, hop_anchors);
    for (size_t slot = 0; slot < ARRAY_SIZE(anchor_channels); slot++) {
        for (uint8_t index = 0; index < HOP_COUNT; index++) {
            if (hop_channels[index] == anchor_channels[slot]) {
                anchor_indices[slot] = index;
                break;
            }
        }
    }
#else
    /* Adjacent anchors carry no frequency diversity. */
    for (uint8_t slot = 0; slot < ESB_HOP_ANCHOR_COUNT; slot++) {
        anchor_indices[slot] =
            (uint8_t)((HOP_COUNT * (2u * slot + 1u)) / (2u * ESB_HOP_ANCHOR_COUNT));
    }
#endif
    anchors_resolved = true;
}

uint8_t hop_anchor_index_at(uint8_t slot) {
    resolve_anchors();
    return anchor_indices[slot];
}

bool hop_is_anchor_index(uint8_t index) {
    resolve_anchors();
    for (uint8_t slot = 0; slot < ESB_HOP_ANCHOR_COUNT; slot++) {
        if (anchor_indices[slot] == index) {
            return true;
        }
    }
    return false;
}
