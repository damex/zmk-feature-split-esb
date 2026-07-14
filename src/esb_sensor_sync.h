// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Sensor rotation crosses the air as a cumulative microdegree total. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESB_SENSOR_MICRODEG_PER_DEG 1000000LL
/* Gap past this resyncs the baseline instead of replaying stale motion. */
#define ESB_SENSOR_RESYNC_UDEG (180 * ESB_SENSOR_MICRODEG_PER_DEG)

struct esb_sensor_track {
    int64_t last_udeg;
    bool valid;
};

int64_t esb_sensor_udeg(int32_t val1, int32_t val2);

int32_t esb_sensor_udeg_val1(int64_t udeg);

int32_t esb_sensor_udeg_val2(int64_t udeg);

/* Adopts total as the new baseline either way.
 * Returns false when nothing forwards: first sample, zero delta, gap past the cap. */
bool esb_sensor_track_delta(struct esb_sensor_track *track, int64_t total_udeg,
                            int64_t *delta_udeg);
