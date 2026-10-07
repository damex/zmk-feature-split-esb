// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Fake esb_survey.c, every channel reads quiet. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "esb_survey.h"

void esb_survey_run(const uint8_t *channels, size_t count, int8_t *energy_dbm) {
    ARG_UNUSED(channels);
    memset(energy_dbm, INT8_MIN, count);
}
