// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Central hop engine: vote-driven coordinated hopping, epoch beacons, adaptive channel masking.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include "esb_keepalive.h"
#include "esb_link.h"
#include "hop.h"
#include "hop_internal.h"
#include "hop_policy.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define ESB_PERIPHERALS DT_INST_CHILD(0, peripherals)
#define PERIPHERAL_WEIGHT(node) [DT_PROP(node, pipe)] = DT_PROP(node, weight),
static const uint8_t pipe_weights[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, PERIPHERAL_WEIGHT)
};
#define PERIPHERAL_COUNT ARRAY_SIZE(pipe_weights)
static const uint16_t vote_threshold = DT_INST_PROP(0, hop_threshold);
static const uint16_t decision_ms = DT_INST_PROP(0, idle_keepalive_ms);
static const int8_t rssi_floor_dbm = DT_INST_PROP(0, rssi_floor_dbm);
static const uint16_t mask_threshold = DT_INST_PROP(0, hop_mask_threshold);
static const uint16_t restore_windows = DT_INST_PROP(0, hop_restore_windows);
static const uint8_t min_active = DT_INST_PROP(0, hop_min_active);
#define ANCHOR_FALLBACK_CAP 32 /* windows, covers a full-pool sweep */
#define ANCHOR_FALLBACK_WINDOWS MIN(2 * HOP_COUNT, ANCHOR_FALLBACK_CAP)
#define BEACON_REPEAT_WINDOWS 4
#define BEACON_RSSI_PERIOD_WINDOWS 4
#define MASK_UPDATE_REPEAT_WINDOWS 4
#define MASK_REFRESH_WINDOWS 32
#define CHANNEL_BAD_DECAY 1
/* One pipe silent this long, while others are served, gets anchor dips. */
#define PIPE_LOST_WINDOWS ANCHOR_FALLBACK_WINDOWS
/* Past here pipe is gone, not lost.
 * Stop dipping so others run clean. */
#define PIPE_ABSENT_WINDOWS (4 * PIPE_LOST_WINDOWS)
/* Radio can't park, so dip to anchor one window in this many. */
#define ANCHOR_VISIT_PERIOD 8
BUILD_ASSERT(ESB_MASK_UPDATE_LENGTH <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "mask update does not fit in one ESB payload");
static uint8_t hop_epoch;
static uint8_t pipe_loss[PERIPHERAL_COUNT];
static int8_t pipe_rssi_dbm[PERIPHERAL_COUNT];
static atomic_t pipe_heard_mask;
static atomic_t pipe_motion_mask;
static atomic_t pipe_active_mask;
static uint16_t silent_windows;
static uint8_t pipe_silent[PERIPHERAL_COUNT];
static uint16_t anchor_visit_window;
static bool in_anchor_visit;
static uint8_t beaconed_epoch;
static uint8_t beacon_repeats_left;
static uint8_t beacon_window;
static uint8_t channel_bad[HOP_COUNT];
static uint16_t channel_masked_windows[HOP_COUNT];
static uint8_t active_mask[ESB_HOP_MASK_BYTES];
static uint8_t pending_mask[ESB_HOP_MASK_BYTES];
static bool pending_valid;
static bool mask_ready;
static uint8_t mask_version;
static uint8_t mask_update_repeats;
static uint16_t mask_window;

static void clear_pipe_loss(void) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        pipe_loss[pipe] = 0;
    }
}

static bool pipe_is_lost(uint8_t pipe) {
    return pipe_silent[pipe] >= PIPE_LOST_WINDOWS && pipe_silent[pipe] < PIPE_ABSENT_WINDOWS;
}

static void update_pipe_silence(uint32_t heard) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (heard & BIT(pipe)) {
            pipe_silent[pipe] = 0;
        } else if (pipe_silent[pipe] < UINT8_MAX) {
            pipe_silent[pipe]++;
        }
    }
}

static void clear_silence_for_heard(uint32_t heard) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (heard & BIT(pipe)) {
            pipe_silent[pipe] = 0;
        }
    }
}

static bool any_pipe_lost(void) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (pipe_is_lost(pipe)) {
            return true;
        }
    }
    return false;
}

/* Beacons the live epoch to lost pipes, so one camped on the anchor reads it
 * from its poll ACK and rejoins the hop. */
static void stage_anchor_beacon(void) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (!pipe_is_lost(pipe)) {
            continue;
        }
        struct esb_beacon beacon = {.tag = ESB_BEACON_TAG,
                                    .epoch = hop_epoch,
                                    .rssi_dbm = pipe_rssi_dbm[pipe],
                                    .mask_version = mask_version};
        (void)esb_link_stage_reply(pipe, (const uint8_t *)&beacon, sizeof(beacon));
    }
}

static void ensure_mask(void) {
    if (mask_ready) {
        return;
    }
    for (size_t channel = 0; channel < HOP_COUNT; channel++) {
        hop_policy_mask_set(active_mask, channel, true);
        hop_policy_mask_set(pending_mask, channel, true);
    }
    mask_ready = true;
}

/* Mask swap rides the epoch transition: both ends switch in lockstep, not central-first. */
static void commit_pending_mask(void) {
    if (pending_valid) {
        memcpy(active_mask, pending_mask, ESB_HOP_MASK_BYTES);
        pending_valid = false;
    }
}

static void hop_to_next_epoch(void) {
    commit_pending_mask();
    hop_epoch++;
    hop_index = hop_policy_channel_for_epoch_masked(hop_epoch, active_mask, HOP_COUNT);
    apply_hop_channel();
    clear_pipe_loss();
}

static void fall_back_to_anchor(void) {
    commit_pending_mask();
    hop_epoch = 0;
    hop_index = hop_policy_channel_for_epoch_masked(0, active_mask, HOP_COUNT);
    apply_hop_channel();
    silent_windows = 0;
    clear_pipe_loss();
}

static void stage_beacon(uint32_t heard) {
    bool burst = hop_policy_should_beacon(hop_epoch, &beaconed_epoch, &beacon_repeats_left,
                                          BEACON_REPEAT_WINDOWS);
    bool refresh = (++beacon_window % BEACON_RSSI_PERIOD_WINDOWS) == 0;
    if (!burst && !refresh) {
        return;
    }
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (!burst && !(heard & BIT(pipe))) {
            continue;
        }
        struct esb_beacon beacon = {.tag = ESB_BEACON_TAG,
                                    .epoch = hop_epoch,
                                    .rssi_dbm = pipe_rssi_dbm[pipe],
                                    .mask_version = mask_version};
        (void)esb_link_stage_reply(pipe, (const uint8_t *)&beacon, sizeof(beacon));
    }
}

static void score_current_channel(uint32_t motion, uint32_t active) {
    if (active == 0) {
        return;
    }
    uint8_t penalty = hop_policy_window_penalty(motion, active, pipe_rssi_dbm, rssi_floor_dbm,
                                                PERIPHERAL_COUNT);
    uint8_t *score = &channel_bad[hop_index];
    if (penalty == 0) {
        *score = (*score > CHANNEL_BAD_DECAY) ? (uint8_t)(*score - CHANNEL_BAD_DECAY) : 0;
    } else if (*score > UINT8_MAX - penalty) {
        *score = UINT8_MAX;
    } else {
        *score = (uint8_t)(*score + penalty);
    }
}

/* Writes pending, not active: commit_pending_mask applies it at the next hop. */
static void recompute_mask(void) {
    ensure_mask();
    bool changed = false;
    for (size_t channel = 0; channel < HOP_COUNT; channel++) {
        if (hop_policy_mask_get(pending_mask, channel)) {
            continue;
        }
        if (++channel_masked_windows[channel] >= restore_windows) {
            hop_policy_mask_set(pending_mask, channel, true);
            channel_bad[channel] = 0;
            channel_masked_windows[channel] = 0;
            changed = true;
            LOG_INF("afh: ch %u back to retest", (unsigned)hop_channel_at((uint8_t)channel));
        }
    }
    if (hop_policy_mask_active_count(pending_mask, HOP_COUNT) > min_active) {
        int worst = -1;
        for (size_t channel = 0; channel < HOP_COUNT; channel++) {
            if (channel == ESB_HOP_ANCHOR_INDEX) {
                continue;
            }
            if (hop_policy_mask_get(pending_mask, channel) && channel_bad[channel] >= mask_threshold
                && (worst < 0 || channel_bad[channel] > channel_bad[worst])) {
                worst = (int)channel;
            }
        }
        if (worst >= 0) {
            hop_policy_mask_set(pending_mask, (size_t)worst, false);
            channel_masked_windows[worst] = 0;
            changed = true;
            LOG_INF("afh: ch %u masked, score %u, %u active", (unsigned)hop_channel_at((uint8_t)worst),
                    (unsigned)channel_bad[worst],
                    (unsigned)hop_policy_mask_active_count(pending_mask, HOP_COUNT));
        }
    }
    if (changed) {
        mask_version++;
        pending_valid = true;
        mask_update_repeats = MASK_UPDATE_REPEAT_WINDOWS;
    }
}

/* Slow refresh backs the post-change burst, so a peripheral that missed it still converges. */
static void stage_mask_update(void) {
    bool burst = mask_update_repeats > 0;
    bool refresh = (++mask_window % MASK_REFRESH_WINDOWS) == 0;
    if (burst) {
        mask_update_repeats--;
    } else if (!refresh) {
        return;
    }
    const uint8_t *mask = pending_valid ? pending_mask : active_mask;
    struct esb_mask_update update = {.tag = ESB_MASK_UPDATE_TAG, .version = mask_version};
    memcpy(update.mask, mask, ESB_HOP_MASK_BYTES);
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (pipe_silent[pipe] >= PIPE_LOST_WINDOWS) {
            continue; /* rejoins via anchor beacon, not a stale-channel mask reply */
        }
        (void)esb_link_stage_reply(pipe, (const uint8_t *)&update, ESB_MASK_UPDATE_LENGTH);
    }
}

/* Hopping tracks poll traffic, not a keepalive timer: only an actively-polling pipe whose
 * motion goes missing accrues loss, so an idle or absent peripheral never drives a hop.
 * A weighted vote over that loss hops to escape a degrading channel.
 * Losing every pipe for too long falls back to the anchor so a sweeping peripheral can
 * re-find it.
 *
 * Statically initialized: ZMK's split central_init and ours share a SYS_INIT level
 * and priority, so set_enabled() can reschedule this work before our init would have
 * run k_work_init_delayable on it. */
static void decision_work_fn(struct k_work *work);
static struct k_work_delayable decision_work = Z_WORK_DELAYABLE_INITIALIZER(decision_work_fn);
static void decision_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t heard = (uint32_t)atomic_set(&pipe_heard_mask, 0);
    uint32_t motion = (uint32_t)atomic_set(&pipe_motion_mask, 0);
    uint32_t active = (uint32_t)atomic_set(&pipe_active_mask, 0);

    if (in_anchor_visit) {
        /* Elapsed window sat on the anchor, not the live channel.
         * Don't score it or fault served pipes for the gap.
         * A lost pipe heard here has rejoined. */
        in_anchor_visit = false;
        apply_hop_channel();
        clear_silence_for_heard(heard);
        k_work_reschedule(&decision_work, K_MSEC(decision_ms));
        return;
    }

    hop_policy_accrue_loss(pipe_loss, PERIPHERAL_COUNT, motion, active, pipe_rssi_dbm, rssi_floor_dbm);
    score_current_channel(motion, active);
    recompute_mask();
    update_pipe_silence(heard);
    if (heard != 0) {
        silent_windows = 0;
    } else {
        silent_windows++;
    }
    if (hop_policy_hop_vote(pipe_loss, pipe_weights, PERIPHERAL_COUNT, vote_threshold)) {
        hop_to_next_epoch();
    }
    if (silent_windows >= ANCHOR_FALLBACK_WINDOWS) {
        fall_back_to_anchor();
    } else if (any_pipe_lost() && (++anchor_visit_window % ANCHOR_VISIT_PERIOD) == 0) {
        in_anchor_visit = true;
        stage_anchor_beacon();
        apply_channel_index(ESB_HOP_ANCHOR_INDEX);
    }
    stage_beacon(heard);
    stage_mask_update();
    k_work_reschedule(&decision_work, K_MSEC(decision_ms));
}

void hop_start(void) {
    if (HOP_COUNT <= 1) {
        return;
    }
    k_work_reschedule(&decision_work, K_MSEC(decision_ms));
}

void hop_stop(void) {
    if (HOP_COUNT <= 1) {
        return;
    }
    k_work_cancel_delayable(&decision_work);
}

bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi) {
    bool keepalive = esb_keepalive_matches(data, length);
    if (pipe < PERIPHERAL_COUNT && !keepalive) {
        /* Store before the motion bit: the decision tick reads pipe_rssi_dbm only when
         * that bit is set, so publish the value first. */
        pipe_rssi_dbm[pipe] = hop_policy_rssi_to_dbm(rssi);
    }
    if (HOP_COUNT <= 1) {
        return false;
    }
    if (pipe < PERIPHERAL_COUNT) {
        atomic_or(&pipe_heard_mask, BIT(pipe));
        if (keepalive) {
            if (hop_policy_keepalive_is_active(esb_keepalive_state(data))) {
                atomic_or(&pipe_active_mask, BIT(pipe));
            }
        } else {
            atomic_or(&pipe_active_mask, BIT(pipe));
            atomic_or(&pipe_motion_mask, BIT(pipe));
        }
    }
    return false;
}

void hop_note_tx_success(uint8_t attempts) {
    ARG_UNUSED(attempts);
}

void hop_note_tx_failed(void) {
}

void hop_note_data_sent(void) {
}

static int8_t worst_pipe_rssi_dbm(void) {
    int8_t worst = 0;
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (pipe_rssi_dbm[pipe] < worst) {
            worst = pipe_rssi_dbm[pipe];
        }
    }
    return worst;
}

void zmk_split_esb_get_status(struct zmk_split_esb_status *status) {
    __ASSERT_NO_MSG(status != NULL);
    status->channel = hop_current_channel();
    status->epoch = hop_epoch;
    status->searching = silent_windows > 0;
    status->rssi_dbm = worst_pipe_rssi_dbm();
}

uint8_t zmk_split_esb_pipe_count(void) {
    return (uint8_t)PERIPHERAL_COUNT;
}

int8_t zmk_split_esb_pipe_rssi_dbm(uint8_t pipe) {
    if (pipe >= PERIPHERAL_COUNT) {
        return 0;
    }
    return pipe_rssi_dbm[pipe];
}
