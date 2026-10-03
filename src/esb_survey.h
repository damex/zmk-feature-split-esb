// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Boot energy survey over the hop pool. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Ambient energy per channel (dBm), several passes, max held.
 * Radio must be idle: run before esb_init, HFCLK running. */
void esb_survey_run(const uint8_t *channels, size_t count, int8_t *energy_dbm);
