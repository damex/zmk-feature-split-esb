// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Channel-hopping engine.
 * Pure decisions live in hop_policy.c, the radio transport in esb_link.c.
 * This file holds the stateful glue: the per-role hop state, the periodic work that
 * drives it, and the radio retune.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "esb_link.h"
#include "hop.h"
#include "hop_policy.h"

static const uint8_t hop_channels[] = DT_INST_PROP(0, hop_channels);
#define HOP_COUNT ARRAY_SIZE(hop_channels)
BUILD_ASSERT(HOP_COUNT >= 1, "hop-channels needs at least one channel");
static uint8_t hop_index;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define ESB_PERIPHERALS DT_INST_CHILD(0, peripherals)
#define PERIPHERAL_WEIGHT(node) [DT_PROP(node, pipe)] = DT_PROP(node, weight),
static const uint8_t pipe_weights[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, PERIPHERAL_WEIGHT)
};
#define PERIPHERAL_COUNT ARRAY_SIZE(pipe_weights)
static const uint16_t vote_threshold = DT_INST_PROP(0, hop_threshold);
static const uint16_t decision_ms = DT_INST_PROP(0, idle_keepalive_ms);
static const int8_t rssi_floor_dbm = DT_INST_PROP(0, rssi_floor_dbm);
#define LOST_LIMIT (2 * HOP_COUNT)  /* silent windows before falling back to the anchor */
#define BEACON_REPEAT_WINDOWS 4     /* re-announce a changed epoch for this many windows */
static uint8_t hop_epoch;
static uint8_t pipe_loss[PERIPHERAL_COUNT]; /* graded degradation, per pipe */
static int8_t pipe_rssi[PERIPHERAL_COUNT];  /* last motion RSSI this window (dBm), per pipe */
static atomic_t pipe_heard_mask;   /* any packet this window, bit per pipe, set in the ISR */
static atomic_t pipe_motion_mask;  /* poll/motion packet this window, bit per pipe */
static atomic_t pipe_active_mask;  /* peripheral polling (motion or active keepalive) */
static uint16_t silent_run;        /* windows since any pipe was heard */
static uint8_t beaconed_epoch;     /* epoch last announced to peripherals */
static uint8_t beacon_repeats_left;
#else
static const uint16_t hop_threshold = DT_INST_PROP(0, hop_threshold);
static const uint16_t hop_window_ms = DT_INST_PROP(0, hop_window_ms);
static const uint16_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);
static atomic_t fails_in_window;
static atomic_t data_sent_since_tick;
static atomic_t beacon_epoch;  /* latest epoch from the central, set in the ISR */
static uint8_t bad_windows;    /* keepalive_work only, sweep streak */
static uint8_t adopted_epoch;  /* keepalive_work only */
#endif

/* Retune with the ISR masked: esb_stop_rx() briefly nulls the radio disabled-callback,
 * and a DISABLED event landing there mid-reconfigure faults the device.
 * Skip the work when the channel is unchanged, so a redundant retune never tears down RX. */
static uint8_t applied_index;
static void apply_hop_channel(void) {
    if (hop_index == applied_index) {
        return;
    }
    applied_index = hop_index;
    unsigned int key = irq_lock();
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    esb_stop_rx();
    esb_set_rf_channel(hop_channels[hop_index]);
    esb_start_rx();
#else
    esb_set_rf_channel(hop_channels[hop_index]); /* PTX: no RX to stop */
#endif
    irq_unlock(key);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
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
    silent_run = 0;
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
    hop_policy_accrue_loss(pipe_loss, PERIPHERAL_COUNT, motion, active, pipe_rssi, rssi_floor_dbm);
    if (heard != 0) {
        silent_run = 0;
    } else {
        silent_run++;
    }
    if (hop_policy_hop_vote(pipe_loss, pipe_weights, PERIPHERAL_COUNT, vote_threshold)) {
        hop_to_next_epoch();
    }
    if (silent_run >= LOST_LIMIT) {
        fall_back_to_anchor();
    }
    stage_beacon();
    k_work_reschedule(&decision_work, K_MSEC(decision_ms));
}
#else
/* Adopt the central's channel when its beacon epoch changes.
 * Otherwise sweep the table on a TX-fail streak to re-find where the central hopped.
 * Statically initialized for the same SYS_INIT-order reason as decision_work. */
static void keepalive_work_fn(struct k_work *work);
static struct k_work_delayable keepalive_work = Z_WORK_DELAYABLE_INITIALIZER(keepalive_work_fn);
static void keepalive_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t epoch = (uint8_t)atomic_get(&beacon_epoch);
    if (epoch != adopted_epoch) {
        adopted_epoch = epoch;
        hop_index = hop_policy_channel_for_epoch(epoch, HOP_COUNT);
        apply_hop_channel();
        bad_windows = 0;
        atomic_set(&fails_in_window, 0);
    } else {
        bool window_failed = atomic_set(&fails_in_window, 0) > 0;
        if (hop_policy_should_hop(&bad_windows, window_failed, hop_threshold)) {
            hop_index = hop_policy_index_next(hop_index, HOP_COUNT);
            apply_hop_channel();
        }
    }
    bool active = atomic_set(&data_sent_since_tick, 0) != 0;
    esb_link_send_keepalive(active ? ESB_KEEPALIVE_ACTIVE : ESB_KEEPALIVE_IDLE);
    k_work_reschedule(&keepalive_work, K_MSEC(active ? hop_window_ms : idle_keepalive_ms));
}
#endif

void hop_start(void) {
    if (HOP_COUNT <= 1) {
        return;
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    k_work_reschedule(&decision_work, K_MSEC(decision_ms));
#else
    k_work_reschedule(&keepalive_work, K_MSEC(hop_window_ms));
#endif
}

void hop_stop(void) {
    if (HOP_COUNT <= 1) {
        return;
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    k_work_cancel_delayable(&decision_work);
#else
    k_work_cancel_delayable(&keepalive_work);
#endif
}

bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi) {
    if (HOP_COUNT <= 1) {
        return false;
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    bool keepalive = hop_policy_is_keepalive(length);
    if (pipe < PERIPHERAL_COUNT) {
        atomic_or(&pipe_heard_mask, BIT(pipe));
        if (keepalive) {
            if (hop_policy_keepalive_is_active(data[0])) {
                atomic_or(&pipe_active_mask, BIT(pipe));
            }
        } else {
            atomic_or(&pipe_active_mask, BIT(pipe));
            atomic_or(&pipe_motion_mask, BIT(pipe));
            /* dBm (ESB gives the magnitude); graded into loss at the decision tick. */
            pipe_rssi[pipe] = (int8_t)(-rssi);
        }
    }
    return keepalive; /* a keepalive marks liveness and isn't queued, motion is */
#else
    ARG_UNUSED(pipe);
    ARG_UNUSED(rssi);
    if (length == ESB_KEEPALIVE_LENGTH) {
        atomic_set(&beacon_epoch, data[0]); /* epoch beacon: adopted in keepalive_work, not queued */
        return true;
    }
    return false;
#endif
}

void hop_note_tx_failed(void) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (HOP_COUNT > 1) {
        atomic_inc(&fails_in_window);
    }
#endif
}

void hop_note_data_sent(void) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (HOP_COUNT > 1) {
        atomic_set(&data_sent_since_tick, 1);
    }
#endif
}

uint8_t hop_current_channel(void) {
    return hop_channels[hop_index];
}
