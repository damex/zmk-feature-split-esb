// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * ESB link core stand-in under a real esb_link_central.c.
 * Mock case links mock_esb.c for the radio below.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdint.h>

#include <zephyr/devicetree.h>

#include "esb_link_internal.h"

const uint8_t esb_link_pipe_count = DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals));
