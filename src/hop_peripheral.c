// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Peripheral hop engine: adopt the central's epoch, sweep to re-find it on a bad uplink.
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

static const uint16_t hop_threshold = DT_INST_PROP(0, hop_threshold);
static const uint16_t hop_window_ms = DT_INST_PROP(0, hop_window_ms);
static const uint16_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);
static atomic_t max_tx_attempts;
static atomic_t data_sent_since_tick;
static atomic_t beacon_epoch;
static uint8_t bad_windows;
static uint8_t adopted_epoch;
static int8_t last_rssi_dbm;

/* Adopt the central's channel when its beacon epoch changes.
 * Otherwise sweep the table on a TX-fail streak to re-find where the central hopped.
 * Statically initialized for the same SYS_INIT-order reason as the central work. */
static void keepalive_work_fn(struct k_work *work);
static struct k_work_delayable keepalive_work = Z_WORK_DELAYABLE_INITIALIZER(keepalive_work_fn);
static void keepalive_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (HOP_COUNT > 1) {
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
    }
    bool active = atomic_set(&data_sent_since_tick, 0) != 0;
    esb_link_send_keepalive(active ? ESB_KEEPALIVE_ACTIVE : ESB_KEEPALIVE_IDLE,
                            esb_link_keepalive_bitmap(), esb_link_keepalive_battery_level());
    k_work_reschedule(&keepalive_work, K_MSEC(active ? hop_window_ms : idle_keepalive_ms));
}

void hop_start(void) {
    k_work_reschedule(&keepalive_work, K_MSEC(hop_window_ms));
}

void hop_stop(void) {
    k_work_cancel_delayable(&keepalive_work);
}

bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi) {
    ARG_UNUSED(pipe);
    last_rssi_dbm = hop_policy_rssi_to_dbm(rssi);
    if (HOP_COUNT <= 1) {
        return false;
    }
    if (hop_policy_is_beacon(length)) {
        uint8_t epoch = data[0];
        atomic_set(&beacon_epoch, epoch); /* adopted in keepalive_work, not queued */
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
    atomic_set(&data_sent_since_tick, 1);
}

void zmk_split_esb_get_status(struct zmk_split_esb_status *status) {
    __ASSERT_NO_MSG(status != NULL);
    status->channel = hop_current_channel();
    status->epoch = adopted_epoch;
    status->searching = bad_windows > 0;
    status->rssi_dbm = last_rssi_dbm;
}

uint8_t zmk_split_esb_pipe_count(void) {
    return 1;
}

int8_t zmk_split_esb_pipe_rssi_dbm(uint8_t pipe) {
    if (pipe >= 1) {
        return 0;
    }
    return last_rssi_dbm;
}
