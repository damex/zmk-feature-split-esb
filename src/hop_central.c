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
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_survey.h"
#include "hop.h"
#include "hop_internal.h"
#include "hop_policy.h"
#include "wire_central.h"

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
static const int8_t survey_threshold_dbm = DT_INST_PROP(0, survey_threshold_dbm);
static const uint16_t mask_threshold = DT_INST_PROP(0, hop_mask_threshold);
static const uint16_t restore_windows = DT_INST_PROP(0, hop_restore_windows);
static const uint8_t min_active = DT_INST_PROP(0, hop_min_active);
#define SILENT_ESCAPE_CAP 32 /* windows, covers a full-pool sweep */
#define SILENT_ESCAPE_WINDOWS MIN(2 * HOP_COUNT, SILENT_ESCAPE_CAP)
/* Escape walks the live channel off a degraded spot.
 * A struggling pipe is heard once its sweep lands and resets the count.
 * A sleeping pipe never is, so the walk stops after one pool sweep, not forever. */
#define SILENT_ESCAPE_LIMIT HOP_COUNT
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
static atomic_t pipe_rssi_dbm[PERIPHERAL_COUNT];
static atomic_t pipe_heard_mask;
static atomic_t pipe_motion_mask;
static atomic_t pipe_active_mask;
static uint16_t silent_windows;
static uint8_t silent_escapes;
static atomic_t pipe_last_heard_ms[PERIPHERAL_COUNT];
static ATOMIC_DEFINE(pipe_ever_heard, PERIPHERAL_COUNT);
static uint32_t pipe_was_lost_mask;
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
static uint8_t mask_update_repeats;
static uint8_t mask_window;
static uint8_t persisted_mask[ESB_HOP_MASK_BYTES];
static bool persisted_mask_valid;

#define MASK_SAVE_DEBOUNCE_SEC 60
#define MASK_STORE_LENGTH HOP_POLICY_MASK_STORE_LENGTH(HOP_COUNT)

static void pool_channels(uint8_t *channels) {
    for (uint8_t index = 0; index < HOP_COUNT; index++) {
        channels[index] = hop_channel_at(index);
    }
}

#if defined(CONFIG_SETTINGS)
static int hop_mask_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                 void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, "mask", &next) || next != NULL) {
        return -ENOENT;
    }
    uint8_t stored[MASK_STORE_LENGTH];
    if (len != sizeof(stored)) {
        return 0;
    }
    if (read_cb(cb_arg, stored, sizeof(stored)) < 0) {
        return -EIO;
    }
    uint8_t channels[HOP_COUNT];
    pool_channels(channels);
    if (!hop_policy_mask_store_matches(stored, sizeof(stored), channels, HOP_COUNT)) {
        return 0;
    }
    memcpy(persisted_mask, hop_policy_mask_store_mask(stored, HOP_COUNT), ESB_HOP_MASK_BYTES);
    persisted_mask_valid = true;
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(esb_hop, "esb_hop", NULL, hop_mask_settings_set, NULL, NULL);

static void mask_save_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t stored[MASK_STORE_LENGTH];
    uint8_t channels[HOP_COUNT];
    pool_channels(channels);
    size_t length =
        hop_policy_mask_store_encode(stored, sizeof(stored), channels, active_mask, HOP_COUNT);
    if (length == 0) {
        return;
    }
    int error = settings_save_one("esb_hop/mask", stored, length);
    if (error < 0) {
        LOG_DBG("mask save failed (%d)", error);
    }
}
static K_WORK_DELAYABLE_DEFINE(mask_save_work, mask_save_work_fn);

static void schedule_mask_save(void) {
    k_work_reschedule(&mask_save_work, K_SECONDS(MASK_SAVE_DEBOUNCE_SEC));
}
#else
static void schedule_mask_save(void) {
}
#endif /* CONFIG_SETTINGS */

static int8_t pipe_rssi_dbm_get(uint8_t pipe) {
    return (int8_t)atomic_get(&pipe_rssi_dbm[pipe]);
}

static void clear_pipe_loss(void) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        pipe_loss[pipe] = 0;
    }
}

uint32_t hop_pipe_quiet_ms(uint8_t pipe) {
    if (pipe >= PERIPHERAL_COUNT) {
        return UINT32_MAX;
    }
    return k_uptime_get_32() - (uint32_t)atomic_get(&pipe_last_heard_ms[pipe]);
}

bool hop_pipe_heard(uint8_t pipe) {
    if (pipe >= PERIPHERAL_COUNT) {
        return false;
    }
    return atomic_test_bit(pipe_ever_heard, pipe);
}

void hop_pipe_note_seen(uint8_t pipe) {
    if (pipe >= PERIPHERAL_COUNT) {
        return;
    }
    atomic_set(&pipe_last_heard_ms[pipe], (atomic_val_t)k_uptime_get_32());
    atomic_set_bit(pipe_ever_heard, pipe);
}

bool hop_pipe_needs_rendezvous(uint8_t pipe) {
    if (pipe >= PERIPHERAL_COUNT) {
        return true;
    }
    if (esb_link_pipe_is_self(pipe) || wire_central_owns_pipe(pipe)) {
        return false;
    }
    return hop_pipe_quiet_ms(pipe) >= ESB_HOP_LOSS_DETECT_MS;
}

int hop_stage_beacon(uint8_t pipe, uint8_t hid_modifiers, uint8_t hid_indicators) {
    if (pipe >= PERIPHERAL_COUNT) {
        return -EINVAL;
    }
    struct esb_beacon beacon = {.tag = ESB_BEACON_TAG,
                                .epoch = hop_epoch,
                                .hid_modifiers = hid_modifiers,
                                .hid_indicators = hid_indicators};
    for (uint8_t peer = 0; peer < PERIPHERAL_COUNT; peer++) {
        beacon.peers[peer].battery = esb_central_battery_level(peer);
        beacon.peers[peer].rssi_dbm = pipe_rssi_dbm_get(peer);
    }
    if (wire_central_owns_pipe(pipe)) {
        return wire_central_send((const uint8_t *)&beacon, sizeof(beacon));
    }
    return esb_link_latch_control(pipe, ESB_LINK_CONTROL_BEACON, (const uint8_t *)&beacon,
                                  sizeof(beacon));
}

static void stage_beacon_to(uint8_t pipe) {
    (void)hop_stage_beacon(pipe, zmk_split_esb_hid_modifiers(), zmk_split_esb_hid_indicators());
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
    LOG_INF("hop: epoch %u channel %u", hop_epoch, hop_current_channel());
}

/* Prolonged total silence: the live channel may have degraded with no active pipe
 * to vote it down. Advance the epoch to escape it.
 * Lost halves sweep onto the new channel. */
static void escape_silent_channel(void) {
    silent_windows = 0;
    hop_to_next_epoch();
}

static bool pipe_heard_in_window(uint8_t pipe, uint32_t heard) {
    if (wire_central_owns_pipe(pipe)) {
        return wire_central_peer_is_up();
    }
    return (heard & BIT(pipe)) != 0;
}

static void stage_beacon(uint32_t heard) {
    bool burst = hop_policy_should_beacon(hop_epoch, &beaconed_epoch, &beacon_repeats_left,
                                          BEACON_REPEAT_WINDOWS);
    bool refresh = hop_policy_window_period_fires(&beacon_window, BEACON_RSSI_PERIOD_WINDOWS);
    if (!burst && !refresh) {
        return;
    }
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (esb_link_pipe_is_self(pipe)) {
            continue;
        }
        if (!burst && !pipe_heard_in_window(pipe, heard)) {
            continue;
        }
        stage_beacon_to(pipe);
    }
}

static void score_current_channel(uint32_t motion, uint32_t active, const int8_t *rssi_dbm) {
    if (active == 0) {
        return;
    }
    uint8_t penalty = hop_policy_window_penalty(motion, active, rssi_dbm, rssi_floor_dbm,
                                                PERIPHERAL_COUNT);
    hop_policy_score_update(&channel_bad[hop_index], penalty, CHANNEL_BAD_DECAY);
}

/* Writes pending, not active: commit_pending_mask applies it at the next hop. */
static void recompute_mask(uint32_t active) {
    ensure_mask();
    bool changed = false;
    for (size_t channel = 0; channel < HOP_COUNT; channel++) {
        if (hop_policy_mask_get(pending_mask, channel)) {
            /* Restore period served live resets the retest backoff.
             * Unvisited pool time proves nothing, a loitering bad channel
             * would reset its own escalation. */
            bool served = channel == hop_index && active != 0;
            if (served && channel_active_windows[channel] < restore_windows) {
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
        pending_valid = true;
        mask_update_repeats = MASK_UPDATE_REPEAT_WINDOWS;
        schedule_mask_save();
    }
}

/* Slow refresh backs the post-change burst, so a peripheral that missed it still converges. */
static void stage_mask_update(void) {
    bool burst = mask_update_repeats > 0;
    bool refresh = hop_policy_window_period_fires(&mask_window, MASK_REFRESH_WINDOWS);
    if (burst) {
        mask_update_repeats--;
    } else if (!refresh) {
        return;
    }
    const uint8_t *mask = pending_valid ? pending_mask : active_mask;
    struct esb_mask_update update = {.tag = ESB_MASK_UPDATE_TAG};
    memcpy(update.mask, mask, ESB_HOP_MASK_BYTES);
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (esb_link_pipe_is_self(pipe) || wire_central_owns_pipe(pipe)) {
            continue;
        }
        if (hop_pipe_needs_rendezvous(pipe)) {
            continue; /* rejoins via its rejoin beacon, not a stale-channel mask reply */
        }
        (void)esb_link_latch_control(pipe, ESB_LINK_CONTROL_MASK, (const uint8_t *)&update,
                                     ESB_MASK_UPDATE_LENGTH);
    }
}

/* Hopping tracks poll traffic, not a keepalive timer: only an actively-polling pipe whose
 * motion goes missing accrues loss, so an idle or absent peripheral never drives a hop.
 * A weighted vote over that loss hops to escape a degrading channel.
 * Losing every pipe for too long walks the live channel once around the pool.
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
        stage_beacon(heard);
        k_work_reschedule(&decision_work, K_MSEC(decision_ms));
        return;
    }

    uint32_t motion = (uint32_t)atomic_set(&pipe_motion_mask, 0);
    uint32_t active = (uint32_t)atomic_set(&pipe_active_mask, 0);

    int8_t rssi_snapshot[PERIPHERAL_COUNT];
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        rssi_snapshot[pipe] = pipe_rssi_dbm_get(pipe);
    }
    hop_policy_accrue_loss(pipe_loss, PERIPHERAL_COUNT, motion, active, rssi_snapshot,
                           rssi_floor_dbm);
    LOG_DBG("hop: heard=%02x motion=%02x active=%02x", (unsigned)heard, (unsigned)motion,
            (unsigned)active);
    score_current_channel(motion, active, rssi_snapshot);
    recompute_mask(active);
    uint32_t rejoining = 0;
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if ((heard & BIT(pipe)) && (pipe_was_lost_mask & BIT(pipe)) != 0) {
            rejoining |= BIT(pipe);
        }
    }
    pipe_was_lost_mask = 0;
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        if (hop_pipe_needs_rendezvous(pipe)) {
            pipe_was_lost_mask |= BIT(pipe);
        }
    }
    if (heard != 0) {
        silent_windows = 0;
        silent_escapes = 0;
    } else {
        silent_windows++;
    }
    if (hop_policy_hop_vote(pipe_loss, pipe_weights, PERIPHERAL_COUNT, vote_threshold)) {
        hop_to_next_epoch();
    }
    if (silent_windows >= SILENT_ESCAPE_WINDOWS && silent_escapes < SILENT_ESCAPE_LIMIT) {
        silent_escapes++;
        escape_silent_channel();
    }
    if (rejoining != 0) {
        stage_rejoin_beacon(rejoining);
    }
    stage_beacon(heard);
    stage_mask_update();
    k_work_reschedule(&decision_work, K_MSEC(decision_ms));
}

void hop_start(void) {
    k_work_reschedule(&decision_work, K_MSEC(decision_ms));
}

void hop_stop(void) {
    k_work_cancel_delayable(&decision_work);
}

void hop_boot_mask(void) {
    if (HOP_COUNT <= 1) {
        return;
    }
    ensure_mask();
    if (persisted_mask_valid) {
        for (size_t channel = 0; channel < HOP_COUNT; channel++) {
            if (!hop_policy_mask_get(persisted_mask, channel) &&
                !hop_policy_mask_get(anchor_mask, channel)) {
                hop_policy_mask_set(pending_mask, channel, false);
            }
        }
    }
    uint8_t channels[HOP_COUNT];
    int8_t energy_dbm[HOP_COUNT];
    pool_channels(channels);
    esb_survey_run(channels, HOP_COUNT, energy_dbm);
    (void)hop_policy_survey_mask(energy_dbm, HOP_COUNT, anchor_mask, min_active,
                                 survey_threshold_dbm, pending_mask);
    if (hop_policy_mask_active_count(pending_mask, HOP_COUNT) == HOP_COUNT) {
        return;
    }
    for (size_t channel = 0; channel < HOP_COUNT; channel++) {
        if (!hop_policy_mask_get(pending_mask, channel)) {
            LOG_INF("boot: channel %u masked (%d dBm)",
                    (unsigned)hop_channel_at((uint8_t)channel), (int)energy_dbm[channel]);
        }
    }
    /* Pre-traffic: adopt in place, esb_link_init tunes to hop_current_channel.
     * Epoch bump makes a peripheral re-adopt, staying at 0 reads as no change. */
    memcpy(active_mask, pending_mask, ESB_HOP_MASK_BYTES);
    mask_update_repeats = MASK_UPDATE_REPEAT_WINDOWS;
    hop_epoch++;
    hop_index = hop_policy_channel_for_epoch_masked(hop_epoch, active_mask, HOP_COUNT);
}

bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi) {
    if (pipe >= PERIPHERAL_COUNT) {
        return false;
    }
    bool keepalive = esb_keepalive_matches(data, length);
    if (!keepalive) {
        /* Store before the motion bit: the decision tick reads pipe_rssi_dbm only when
         * that bit is set, so publish the value first. */
        atomic_set(&pipe_rssi_dbm[pipe], hop_policy_rssi_to_dbm(rssi));
    }
    /* Beacon refresh reads it on a fixed channel too. */
    atomic_or(&pipe_heard_mask, BIT(pipe));
    atomic_set(&pipe_last_heard_ms[pipe], (atomic_val_t)k_uptime_get_32());
    atomic_set_bit(pipe_ever_heard, pipe);
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
        int8_t rssi_dbm = pipe_rssi_dbm_get(pipe);
        if (rssi_dbm < worst) {
            worst = rssi_dbm;
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
    status->attempts_ewma_x10 = 0;
}

uint8_t zmk_split_esb_pipe_count(void) {
    return (uint8_t)PERIPHERAL_COUNT;
}

int8_t zmk_split_esb_pipe_rssi_dbm(uint8_t pipe) {
    if (pipe >= PERIPHERAL_COUNT) {
        return 0;
    }
    return pipe_rssi_dbm_get(pipe);
}

uint8_t zmk_split_esb_peer_battery(uint8_t pipe) {
    if (pipe >= PERIPHERAL_COUNT) {
        return ESB_KEEPALIVE_BATTERY_UNKNOWN;
    }
    return esb_central_battery_level(pipe);
}

int8_t zmk_split_esb_peer_rssi_dbm(uint8_t pipe) {
    if (pipe >= PERIPHERAL_COUNT) {
        return 0;
    }
    return pipe_rssi_dbm_get(pipe);
}
