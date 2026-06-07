// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Shared channel layer between hop.c and the per-role engines (hop_central.c,
 * hop_peripheral.c). The includer must define DT_DRV_COMPAT zmk_split_esb first.
 */
#pragma once

#include <stdint.h>

#include <zephyr/devicetree.h>

#define HOP_COUNT DT_INST_PROP_LEN(0, hop_channels)

/* Current channel index: a role engine sets it, then calls apply_hop_channel. */
extern uint8_t hop_index;

void apply_hop_channel(void);
