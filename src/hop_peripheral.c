// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Peripheral hop engine: adopt the central's epoch and mask, sweep to re-find it on a bad uplink.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include "esb_link.h"
#include "hop.h"
#include "hop_internal.h"
#include "hop_policy.h"

/* No-ack windows before camping on the anchor instead of single-stepping. */
#define CAMP_BOUND_CAP 32 /* windows, bounds the bound on a large pool */
#define CAMP_BOUND MIN(2 * HOP_COUNT, CAMP_BOUND_CAP)

static const uint16_t hop_threshold = DT_INST_PROP(0, hop_threshold);
static const uint16_t hop_window_ms = DT_INST_PROP(0, hop_window_ms);
static const uint16_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);
static atomic_t max_tx_attempts;
static atomic_t data_sent_since_tick;
static atomic_t link_acked;
static atomic_t beacon_epoch;
static uint8_t bad_windows;
static uint8_t lost_windows;
static uint8_t adopted_epoch;
static int8_t uplink_rssi_dbm;
static uint8_t active_mask[ESB_HOP_MASK_BYTES];
static bool mask_ready;
static uint8_t adopted_mask_version;
static uint8_t staged_mask[ESB_HOP_MASK_BYTES];
static uint8_t staged_mask_version;
static atomic_t mask_update_seen;

static void ensure_mask(void) {
    if (mask_ready) {
        return;
    }
    for (size_t channel = 0; channel < HOP_COUNT; channel++) {
        hop_policy_mask_set(active_mask, channel, true);
    }
    mask_ready = true;
}

/* Read under the lock the radio ISR stages with.
 * True only when the mask actually changed. */
static bool adopt_staged_mask(void) {
    if (atomic_get(&mask_update_seen) == 0) {
        return false;
    }
    unsigned int key = irq_lock();
    bool changed = staged_mask_version != adopted_mask_version;
    if (changed) {
        memcpy(active_mask, staged_mask, ESB_HOP_MASK_BYTES);
        adopted_mask_version = staged_mask_version;
    }
    irq_unlock(key);
    return changed;
}

/* Adopt the central's channel on a beacon epoch or mask change.
 * Otherwise sweep the full pool on a TX-fail streak to re-find the central.
 * The full pool is the rendezvous, so a stale mask still recovers.
 * Statically initialized for the same SYS_INIT-order reason as the central work. */
static void keepalive_work_fn(struct k_work *work);
static struct k_work_delayable keepalive_work = Z_WORK_DELAYABLE_INITIALIZER(keepalive_work_fn);
static void keepalive_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (HOP_COUNT > 1) {
        ensure_mask();
        bool mask_changed = adopt_staged_mask();
        uint8_t epoch = (uint8_t)atomic_get(&beacon_epoch);
        if (epoch != adopted_epoch || mask_changed) {
            adopted_epoch = epoch;
            hop_index = hop_policy_channel_for_epoch_masked(epoch, active_mask, HOP_COUNT);
            apply_hop_channel();
            bad_windows = 0;
            lost_windows = 0;
            atomic_set(&max_tx_attempts, 0);
        } else {
            uint8_t attempts = (uint8_t)atomic_set(&max_tx_attempts, 0);
            uint8_t penalty = hop_policy_attempts_penalty(attempts, HOP_POLICY_GOOD_TX_ATTEMPTS);
            if (atomic_get(&link_acked) != 0) {
                lost_windows = 0;
            } else if (lost_windows < UINT8_MAX) {
                lost_windows++;
            }
            if (lost_windows >= CAMP_BOUND) {
                /* Single-stepping rarely lands on a hopping central.
                 * Wait on the anchor for its periodic dip instead. */
                if (hop_index != ESB_HOP_ANCHOR_INDEX) {
                    hop_index = ESB_HOP_ANCHOR_INDEX;
                    apply_hop_channel();
                }
            } else if (hop_policy_should_hop(&bad_windows, penalty, hop_threshold)) {
                hop_index = hop_policy_index_next(hop_index, HOP_COUNT);
                apply_hop_channel();
            }
        }
    }
    bool active = atomic_set(&data_sent_since_tick, 0) != 0;
    bool searching = atomic_get(&link_acked) == 0;
    uint16_t period_ms = (active || searching) ? hop_window_ms : idle_keepalive_ms;
    esb_link_send_keepalive(active ? ESB_KEEPALIVE_ACTIVE : ESB_KEEPALIVE_IDLE,
                            esb_link_keepalive_bitmap());
    k_work_reschedule(&keepalive_work, K_MSEC(period_ms));
}

void hop_start(void) {
    k_work_reschedule(&keepalive_work, K_MSEC(hop_window_ms));
}

void hop_stop(void) {
    k_work_cancel_delayable(&keepalive_work);
}

bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi) {
    ARG_UNUSED(pipe);
    ARG_UNUSED(rssi);
    if (HOP_COUNT <= 1) {
        return false;
    }
    if (hop_policy_is_beacon(data, length)) {
        const struct esb_beacon *beacon = (const struct esb_beacon *)data;
        atomic_set(&beacon_epoch, beacon->epoch); /* adopted in keepalive_work, not queued */
        uplink_rssi_dbm = beacon->rssi_dbm;
        return true;
    }
    if (esb_is_mask_update(data, length)) {
        const struct esb_mask_update *update = (const struct esb_mask_update *)data;
        memcpy(staged_mask, update->mask, ESB_HOP_MASK_BYTES);
        staged_mask_version = update->version; /* applied under lock in keepalive_work */
        atomic_set(&mask_update_seen, 1);
        return true;
    }
    return false;
}

static void record_tx_attempts(uint8_t attempts) {
    atomic_val_t current = atomic_get(&max_tx_attempts);
    while ((uint8_t)current < attempts && !atomic_cas(&max_tx_attempts, current, attempts)) {
        current = atomic_get(&max_tx_attempts);
    }
}

void hop_note_tx_success(uint8_t attempts) {
    atomic_set(&link_acked, 1);
    if (HOP_COUNT > 1) {
        record_tx_attempts(attempts);
    }
}

void hop_note_tx_failed(void) {
    atomic_set(&link_acked, 0);
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
    status->searching = atomic_get(&link_acked) == 0;
    status->rssi_dbm = uplink_rssi_dbm;
}

uint8_t zmk_split_esb_pipe_count(void) {
    return 1;
}

int8_t zmk_split_esb_pipe_rssi_dbm(uint8_t pipe) {
    if (pipe >= 1) {
        return 0;
    }
    return uplink_rssi_dbm;
}
