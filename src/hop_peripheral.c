// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Peripheral hop engine: adopt the central's epoch, sweep to re-find it on a bad uplink.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "esb_link.h"
#include "hop.h"
#include "hop_internal.h"
#include "hop_policy.h"

static const uint16_t hop_threshold = DT_INST_PROP(0, hop_threshold);
static const uint16_t hop_window_ms = DT_INST_PROP(0, hop_window_ms);
static const uint16_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);
static atomic_t max_tx_attempts; /* worst acked-TX retransmit count this window */
static atomic_t data_sent_since_tick;
static atomic_t beacon_epoch;  /* latest epoch from the central, set in the ISR */
static uint8_t bad_windows;    /* keepalive_work only, sweep streak */
static uint8_t adopted_epoch;  /* keepalive_work only */

/* Adopt the central's channel when its beacon epoch changes.
 * Otherwise sweep the table on a TX-fail streak to re-find where the central hopped.
 * Statically initialized for the same SYS_INIT-order reason as the central work. */
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
        atomic_set(&max_tx_attempts, 0);
    } else {
        uint8_t attempts = (uint8_t)atomic_set(&max_tx_attempts, 0);
        uint8_t penalty = hop_policy_attempts_penalty(attempts, HOP_POLICY_GOOD_TX_ATTEMPTS);
        if (hop_policy_should_hop(&bad_windows, penalty, hop_threshold)) {
            hop_index = hop_policy_index_next(hop_index, HOP_COUNT);
            apply_hop_channel();
        }
    }
    bool active = atomic_set(&data_sent_since_tick, 0) != 0;
    esb_link_send_keepalive(active ? ESB_KEEPALIVE_ACTIVE : ESB_KEEPALIVE_IDLE);
    k_work_reschedule(&keepalive_work, K_MSEC(active ? hop_window_ms : idle_keepalive_ms));
}

void hop_start(void) {
    if (HOP_COUNT <= 1) {
        return;
    }
    k_work_reschedule(&keepalive_work, K_MSEC(hop_window_ms));
}

void hop_stop(void) {
    if (HOP_COUNT <= 1) {
        return;
    }
    k_work_cancel_delayable(&keepalive_work);
}

bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi) {
    ARG_UNUSED(pipe);
    ARG_UNUSED(rssi);
    if (HOP_COUNT <= 1) {
        return false;
    }
    if (length == ESB_KEEPALIVE_LENGTH) {
        atomic_set(&beacon_epoch, data[0]); /* epoch beacon: adopted in keepalive_work, not queued */
        return true;
    }
    return false;
}

static void record_tx_attempts(uint8_t attempts) {
    if ((uint8_t)atomic_get(&max_tx_attempts) < attempts) {
        atomic_set(&max_tx_attempts, attempts);
    }
}

void hop_note_tx_success(uint8_t attempts) {
    if (HOP_COUNT > 1) {
        record_tx_attempts(attempts);
    }
}

void hop_note_tx_failed(void) {
    if (HOP_COUNT > 1) {
        record_tx_attempts(0xFF); /* a lost packet is the worst this window */
    }
}

void hop_note_data_sent(void) {
    if (HOP_COUNT > 1) {
        atomic_set(&data_sent_since_tick, 1);
    }
}
