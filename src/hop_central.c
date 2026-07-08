// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Central hop engine: vote-driven coordinated hopping, epoch beacons, adaptive channel masking.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <errno.h>
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
/* One peripheral poll: shorter and a camped half is never heard during the dip. */
static const uint16_t anchor_dwell_ms = DT_INST_PROP(0, hop_window_ms);
static const int8_t rssi_floor_dbm = DT_INST_PROP(0, rssi_floor_dbm);
static const uint16_t mask_threshold = DT_INST_PROP(0, hop_mask_threshold);
static const uint16_t restore_windows = DT_INST_PROP(0, hop_restore_windows);
static const uint8_t min_active = DT_INST_PROP(0, hop_min_active);
#define ANCHOR_FALLBACK_CAP 32 /* windows, covers a full-pool sweep */
#define ANCHOR_FALLBACK_WINDOWS MIN(2 * HOP_COUNT, ANCHOR_FALLBACK_CAP)
/* Escape walks the live channel off a degraded spot.
 * A struggling pipe is heard on an anchor dip and resets the count.
 * A sleeping pipe never is, so the walk stops after one pool sweep, not forever. */
#define SILENT_ESCAPE_LIMIT HOP_COUNT
#define DIP_AFTER_HOP_WINDOWS 24
#define BEACON_REPEAT_WINDOWS 4
#define BEACON_RSSI_PERIOD_WINDOWS 4
#define MASK_UPDATE_REPEAT_WINDOWS 4
#define MASK_REFRESH_WINDOWS 32
#define CHANNEL_BAD_DECAY 1
BUILD_ASSERT(ESB_MASK_UPDATE_LENGTH <= ESB_LINK_CONTROL_MAX_LENGTH,
             "mask update does not fit one control latch; raise ESB_LINK_CONTROL_MAX_LENGTH");
BUILD_ASSERT(ESB_BEACON_LENGTH <= ESB_LINK_CONTROL_MAX_LENGTH,
             "beacon does not fit one control latch; raise ESB_LINK_CONTROL_MAX_LENGTH");
static uint8_t hop_epoch;
static uint8_t pipe_loss[PERIPHERAL_COUNT];
static int8_t pipe_rssi_dbm[PERIPHERAL_COUNT];
static atomic_t pipe_heard_mask;
static atomic_t pipe_motion_mask;
static atomic_t pipe_active_mask;
static uint16_t silent_windows;
static uint8_t silent_escapes;
static uint8_t pipe_silent[PERIPHERAL_COUNT];
static uint16_t anchor_visit_window;
static uint8_t rendezvous_anchor;
static uint16_t windows_since_hop;
static bool in_anchor_visit;
static uint8_t beaconed_epoch;
static uint8_t beacon_repeats_left;
static uint8_t beacon_window;
static uint8_t channel_bad[HOP_COUNT];
static uint16_t channel_masked_windows[HOP_COUNT];
static uint16_t channel_active_windows[HOP_COUNT];
static uint8_t channel_retest_level[HOP_COUNT];
static uint8_t active_mask[ESB_HOP_MASK_BYTES];
static uint8_t pending_mask[ESB_HOP_MASK_BYTES];
static uint8_t anchor_mask[ESB_HOP_MASK_BYTES];
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

bool hop_pipe_needs_rendezvous(uint8_t pipe) {
    if (pipe >= PERIPHERAL_COUNT) {
        return true;
    }
    return pipe_silent[pipe] >= ESB_HOP_LOST_WINDOWS;
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

static bool any_pipe_needs_rendezvous(void) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (hop_pipe_needs_rendezvous(pipe)) {
            return true;
        }
    }
    return false;
}

/* A still-served pipe pays dip jitter, so slow the rendezvous cadence only when one exists. */
static bool any_pipe_served(void) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (!hop_pipe_needs_rendezvous(pipe)) {
            return true;
        }
    }
    return false;
}

int hop_stage_beacon(uint8_t pipe, uint8_t hid_modifiers, uint8_t hid_indicators) {
    if (pipe >= PERIPHERAL_COUNT) {
        return -EINVAL;
    }
    struct esb_beacon beacon = {.tag = ESB_BEACON_TAG,
                                .epoch = hop_epoch,
                                .rssi_dbm = pipe_rssi_dbm[pipe],
                                .mask_version = mask_version,
                                .hid_modifiers = hid_modifiers,
                                .hid_indicators = hid_indicators};
    return esb_link_latch_control(pipe, ESB_LINK_CONTROL_BEACON, (const uint8_t *)&beacon,
                                  sizeof(beacon));
}

static void stage_beacon_to(uint8_t pipe) {
    (void)hop_stage_beacon(pipe, zmk_split_esb_hid_modifiers(), zmk_split_esb_hid_indicators());
}

/* Beacons the live epoch to lost pipes, so one camped on the anchor reads it
 * from its poll ACK and rejoins the hop. */
static void stage_anchor_beacon(void) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (hop_pipe_needs_rendezvous(pipe)) {
            stage_beacon_to(pipe);
        }
    }
}

/* Beacon a just-heard lost pipe at once, so a peripheral sweeping the live channel
 * adopts the epoch without waiting for the periodic refresh. */
static void stage_rejoin_beacon(uint32_t rejoining) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (rejoining & BIT(pipe)) {
            stage_beacon_to(pipe);
        }
    }
}

static void ensure_mask(void) {
    if (mask_ready) {
        return;
    }
    for (size_t channel = 0; channel < HOP_COUNT; channel++) {
        hop_policy_mask_set(active_mask, channel, true);
        hop_policy_mask_set(pending_mask, channel, true);
        if (hop_is_anchor_index((uint8_t)channel)) {
            hop_policy_mask_set(anchor_mask, channel, true);
        }
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
    windows_since_hop = 0;
    LOG_INF("hop: epoch %u channel %u", hop_epoch, hop_current_channel());
}

/* Prolonged total silence: the live channel may have degraded with no active pipe
 * to vote it down. Advance the epoch to escape it.
 * Rejoin still rides the anchor dips. */
static void escape_silent_channel(void) {
    silent_windows = 0;
    hop_to_next_epoch();
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
        stage_beacon_to(pipe);
    }
}

static void score_current_channel(uint32_t motion, uint32_t active) {
    if (active == 0) {
        return;
    }
    uint8_t penalty = hop_policy_window_penalty(motion, active, pipe_rssi_dbm, rssi_floor_dbm,
                                                PERIPHERAL_COUNT);
    hop_policy_score_update(&channel_bad[hop_index], penalty, CHANNEL_BAD_DECAY);
}

/* Writes pending, not active: commit_pending_mask applies it at the next hop. */
static void recompute_mask(void) {
    ensure_mask();
    bool changed = false;
    for (size_t channel = 0; channel < HOP_COUNT; channel++) {
        if (hop_policy_mask_get(pending_mask, channel)) {
            /* Full restore period survived unmasked: proven clean. */
            if (channel_active_windows[channel] < restore_windows) {
                if (++channel_active_windows[channel] >= restore_windows) {
                    channel_retest_level[channel] = 0;
                }
            }
            continue;
        }
        uint16_t retest = hop_policy_retest_threshold(restore_windows, channel_retest_level[channel]);
        if (++channel_masked_windows[channel] >= retest) {
            hop_policy_mask_set(pending_mask, channel, true);
            channel_bad[channel] = 0;
            channel_masked_windows[channel] = 0;
            changed = true;
            LOG_INF("afh: channel %u back to retest", (unsigned)hop_channel_at((uint8_t)channel));
        }
    }
    if (hop_policy_mask_active_count(pending_mask, HOP_COUNT) > min_active) {
        size_t worst = hop_policy_worst_channel(channel_bad, pending_mask, anchor_mask, HOP_COUNT,
                                                mask_threshold);
        if (worst < HOP_COUNT) {
            hop_policy_mask_set(pending_mask, worst, false);
            channel_masked_windows[worst] = 0;
            if (channel_active_windows[worst] < restore_windows
                && channel_retest_level[worst] < HOP_POLICY_RETEST_LEVEL_MAX) {
                channel_retest_level[worst]++;
            }
            channel_active_windows[worst] = 0;
            changed = true;
            LOG_INF("afh: channel %u masked, score %u, %u active", (unsigned)hop_channel_at((uint8_t)worst),
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
        if (hop_pipe_needs_rendezvous(pipe)) {
            continue; /* rejoins via anchor beacon, not a stale-channel mask reply */
        }
        (void)esb_link_latch_control(pipe, ESB_LINK_CONTROL_MASK, (const uint8_t *)&update,
                                     ESB_MASK_UPDATE_LENGTH);
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

    if (HOP_COUNT <= 1) {
        update_pipe_silence(heard);
        stage_beacon(heard);
        k_work_reschedule(&decision_work, K_MSEC(decision_ms));
        return;
    }

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
    LOG_DBG("hop: heard=%02x motion=%02x active=%02x", (unsigned)heard, (unsigned)motion,
            (unsigned)active);
    score_current_channel(motion, active);
    recompute_mask();
    uint32_t rejoining = 0;
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if ((heard & BIT(pipe)) && hop_pipe_needs_rendezvous(pipe)) {
            rejoining |= BIT(pipe);
        }
    }
    update_pipe_silence(heard);
    if (heard != 0) {
        silent_windows = 0;
        silent_escapes = 0;
    } else {
        silent_windows++;
    }
    if (windows_since_hop < UINT16_MAX) {
        windows_since_hop++;
    }
    if (hop_policy_hop_vote(pipe_loss, pipe_weights, PERIPHERAL_COUNT, vote_threshold)) {
        hop_to_next_epoch();
    }
    uint16_t next_ms = decision_ms;
    if (silent_windows >= ANCHOR_FALLBACK_WINDOWS && silent_escapes < SILENT_ESCAPE_LIMIT) {
        silent_escapes++;
        escape_silent_channel();
    } else if (any_pipe_needs_rendezvous()) {
        uint16_t dip_period;
        if (windows_since_hop >= DIP_AFTER_HOP_WINDOWS) {
            dip_period = ESB_HOP_DIP_STABLE_PERIOD;
        } else {
            dip_period = any_pipe_served() ? ESB_HOP_DIP_ABSENT_PERIOD : ESB_HOP_DIP_PERIOD;
        }
        if ((++anchor_visit_window % dip_period) == 0) {
            rendezvous_anchor = hop_policy_index_next(rendezvous_anchor, ESB_HOP_ANCHOR_COUNT);
            uint8_t anchor_index = hop_anchor_index_at(rendezvous_anchor);
            in_anchor_visit = true;
            stage_anchor_beacon();
            apply_channel_index(anchor_index);
            next_ms = anchor_dwell_ms;
            LOG_DBG("hop: anchor dip channel %u", (unsigned)hop_channel_at(anchor_index));
        }
    }
    if (rejoining != 0) {
        stage_rejoin_beacon(rejoining);
    }
    stage_beacon(heard);
    stage_mask_update();
    k_work_reschedule(&decision_work, K_MSEC(next_ms));
}

void hop_start(void) {
    k_work_reschedule(&decision_work, K_MSEC(decision_ms));
}

void hop_stop(void) {
    k_work_cancel_delayable(&decision_work);
}

bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi) {
    if (pipe >= PERIPHERAL_COUNT) {
        return false;
    }
    bool keepalive = esb_keepalive_matches(data, length);
    if (!keepalive) {
        /* Store before the motion bit: the decision tick reads pipe_rssi_dbm only when
         * that bit is set, so publish the value first. */
        pipe_rssi_dbm[pipe] = hop_policy_rssi_to_dbm(rssi);
    }
    /* Beacon refresh reads it on a fixed channel too. */
    atomic_or(&pipe_heard_mask, BIT(pipe));
    if (HOP_COUNT <= 1) {
        return false;
    }
    if (keepalive) {
        if (hop_policy_keepalive_is_active(esb_keepalive_state(data))) {
            atomic_or(&pipe_active_mask, BIT(pipe));
        }
    } else {
        atomic_or(&pipe_active_mask, BIT(pipe));
        atomic_or(&pipe_motion_mask, BIT(pipe));
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
