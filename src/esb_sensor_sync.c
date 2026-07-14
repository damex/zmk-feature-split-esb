// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <assert.h>
#include <stddef.h>

#include "esb_sensor_sync.h"

int64_t esb_sensor_udeg(int32_t val1, int32_t val2) {
    return (int64_t)val1 * ESB_SENSOR_MICRODEG_PER_DEG + val2;
}

int32_t esb_sensor_udeg_val1(int64_t udeg) {
    return (int32_t)(udeg / ESB_SENSOR_MICRODEG_PER_DEG);
}

int32_t esb_sensor_udeg_val2(int64_t udeg) {
    return (int32_t)(udeg % ESB_SENSOR_MICRODEG_PER_DEG);
}

bool esb_sensor_track_delta(struct esb_sensor_track *track, int64_t total_udeg,
                            int64_t *delta_udeg) {
    assert(track != NULL);
    assert(delta_udeg != NULL);
    int64_t delta = total_udeg - track->last_udeg;
    bool forward = track->valid && delta != 0 && delta >= -ESB_SENSOR_RESYNC_UDEG &&
                   delta <= ESB_SENSOR_RESYNC_UDEG;

    track->last_udeg = total_udeg;
    track->valid = true;
    *delta_udeg = delta;
    return forward;
}
