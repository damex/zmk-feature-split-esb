// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * ESB link core and channel survey stand-ins under a real esb_link_central.c.
 * Mock case links mock_esb.c for the radio below.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include "esb_link_internal.h"
#include "esb_survey.h"

const uint8_t esb_link_pipe_count = DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals));

void esb_survey_run(const uint8_t *channels, size_t count, int8_t *energy_dbm) {
    ARG_UNUSED(channels);
    memset(energy_dbm, INT8_MIN, count);
}
