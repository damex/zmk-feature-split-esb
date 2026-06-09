// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Central hop engine: vote-driven coordinated hopping plus epoch beacons.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include "esb_link.h"
#include "hop.h"
#include "hop_internal.h"
#include "hop_policy.h"

#define ESB_PERIPHERALS DT_INST_CHILD(0, peripherals)
#define PERIPHERAL_WEIGHT(node) [DT_PROP(node, pipe)] = DT_PROP(node, weight),
static const uint8_t pipe_weights[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, PERIPHERAL_WEIGHT)
};
#define PERIPHERAL_COUNT ARRAY_SIZE(pipe_weights)
static const uint16_t vote_threshold = DT_INST_PROP(0, hop_threshold);
static const uint16_t decision_ms = DT_INST_PROP(0, idle_keepalive_ms);
static const int8_t rssi_floor_dbm = DT_INST_PROP(0, rssi_floor_dbm);
#define ANCHOR_FALLBACK_WINDOWS (2 * HOP_COUNT)
#define BEACON_REPEAT_WINDOWS 4
static uint8_t hop_epoch;
static uint8_t pipe_loss[PERIPHERAL_COUNT];
static int8_t pipe_rssi_dbm[PERIPHERAL_COUNT];
static atomic_t pipe_heard_mask;
static atomic_t pipe_motion_mask;
static atomic_t pipe_active_mask;
static uint16_t silent_windows;
static uint8_t beaconed_epoch;
static uint8_t beacon_repeats_left;

static void clear_pipe_loss(void) {
    for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
        pipe_loss[pipe] = 0;
    }
}

static void hop_to_next_epoch(void) {
    hop_epoch++;
    hop_index = hop_policy_channel_for_epoch(hop_epoch, HOP_COUNT);
    apply_hop_channel();
    clear_pipe_loss();
}

static void fall_back_to_anchor(void) {
    hop_epoch = 0;
    hop_index = hop_policy_channel_for_epoch(0, HOP_COUNT);
    apply_hop_channel();
    silent_windows = 0;
    clear_pipe_loss();
}

/* A missed change is recovered by the peripheral's sweep, so the short repeat suffices. */
static void stage_beacon(void) {
    if (hop_policy_should_beacon(hop_epoch, &beaconed_epoch, &beacon_repeats_left,
                                 BEACON_REPEAT_WINDOWS)) {
        for (uint8_t pipe = 0; pipe < PERIPHERAL_COUNT; pipe++) {
            (void)esb_link_stage_reply(pipe, &hop_epoch, 1);
        }
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
    hop_policy_accrue_loss(pipe_loss, PERIPHERAL_COUNT, motion, active, pipe_rssi_dbm, rssi_floor_dbm);
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
    }
    stage_beacon();
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
    if (HOP_COUNT <= 1) {
        return false;
    }
    bool keepalive = hop_policy_is_keepalive(length);
    if (pipe < PERIPHERAL_COUNT) {
        atomic_or(&pipe_heard_mask, BIT(pipe));
        if (keepalive) {
            if (hop_policy_keepalive_is_active(data[0])) {
                atomic_or(&pipe_active_mask, BIT(pipe));
            }
        } else {
            atomic_or(&pipe_active_mask, BIT(pipe));
            /* Store before the motion bit: the decision tick reads pipe_rssi_dbm only when
             * that bit is set, so publish the value first. */
            pipe_rssi_dbm[pipe] = hop_policy_rssi_to_dbm(rssi);
            atomic_or(&pipe_motion_mask, BIT(pipe));
        }
    }
    return keepalive;
}

/* TX result and data-sent are peripheral-side signals; no-ops on the central. */
void hop_note_tx_success(uint8_t attempts) {
    ARG_UNUSED(attempts);
}

void hop_note_tx_failed(void) {
}

void hop_note_data_sent(void) {
}

void zmk_split_esb_get_status(struct zmk_split_esb_status *status) {
    __ASSERT_NO_MSG(status != NULL);
    status->channel = hop_current_channel();
    status->epoch = hop_epoch;
    status->searching = silent_windows > 0;
    status->rssi_dbm = pipe_rssi_dbm[0];
}
