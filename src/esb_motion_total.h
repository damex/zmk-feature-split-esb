// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Lossy input axis running totals. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/__assert.h>

#define ESB_MOTION_RESYNC_COUNTS 8192

struct esb_motion_track {
    int32_t last_total;
    bool valid;
};

/* Wraps like the int32 on the wire. */
static inline int32_t esb_motion_total_add(int32_t total, int32_t delta) {
    return (int32_t)((uint32_t)total + (uint32_t)delta);
}

static inline bool esb_motion_track_gap(struct esb_motion_track *track, int32_t gap,
                                        int32_t *delta) {
    track->last_total = esb_motion_total_add(track->last_total, gap);
    if (gap == 0 || gap > ESB_MOTION_RESYNC_COUNTS || gap < -ESB_MOTION_RESYNC_COUNTS) {
        return false;
    }
    *delta = gap;
    return true;
}

/* Adopts total as the new baseline either way.
 * Returns false when nothing forwards: first sample, zero delta, gap past the cap. */
static inline bool esb_motion_track_delta(struct esb_motion_track *track, int32_t total,
                                          int32_t *delta) {
    __ASSERT_NO_MSG(track != NULL);
    __ASSERT_NO_MSG(delta != NULL);
    if (!track->valid) {
        track->last_total = total;
        track->valid = true;
        return false;
    }
    return esb_motion_track_gap(track, (int32_t)((uint32_t)total - (uint32_t)track->last_total),
                                delta);
}

/* short_total is the low 16 bits of a total, as the keepalive carries it.
 * Returns false when nothing forwards: no baseline, zero delta, gap past the cap. */
static inline bool esb_motion_track_short_delta(struct esb_motion_track *track,
                                                int16_t short_total, int32_t *delta) {
    __ASSERT_NO_MSG(track != NULL);
    __ASSERT_NO_MSG(delta != NULL);
    if (!track->valid) {
        return false;
    }
    int16_t gap = (int16_t)((uint16_t)short_total - (uint16_t)track->last_total);
    return esb_motion_track_gap(track, gap, delta);
}

static inline bool esb_motion_quiet_enough(uint32_t quiet_ms, uint32_t hop_window_ms) {
    return quiet_ms >= hop_window_ms;
}
