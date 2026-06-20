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
