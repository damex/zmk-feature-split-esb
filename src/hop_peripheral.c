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

#include <esb.h>

#include <zmk_split_esb.h>

#include "esb_keepalive.h"
#include "esb_link.h"
#include "hop.h"
#include "hop_internal.h"
#include "hop_policy.h"

static const uint16_t hop_threshold = DT_INST_PROP(0, hop_threshold);
BUILD_ASSERT(DT_INST_PROP(0, hop_threshold) <= UINT8_MAX,
             "hop-threshold above 255 never fires, sweep streak saturates at UINT8_MAX");

#define ADAPTIVE_RETRANSMITS_MIN 2
static const uint8_t retransmit_ceiling = DT_INST_PROP(0, retransmit_count);
static uint16_t attempts_ewma_x10 = 10;
static uint8_t applied_retransmits = DT_INST_PROP(0, retransmit_count);
static const uint16_t hop_window_ms = DT_INST_PROP(0, hop_window_ms);
static const uint16_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);
static atomic_t max_tx_attempts;
static atomic_t data_sent_since_tick;
static atomic_t link_acked;
static atomic_t beacon_epoch;
static uint8_t bad_windows;
static uint16_t lost_windows;
static uint8_t camp_anchor = ESB_HOP_ANCHOR_COUNT - 1;
static uint16_t camp_dwell;
static uint8_t degrade_undo_index;
static bool degrade_undo_armed;
static uint8_t adopted_epoch;
static volatile int8_t uplink_rssi_dbm;
static uint8_t active_mask[ESB_HOP_MASK_BYTES];
static bool mask_ready;
static uint8_t staged_mask[ESB_HOP_MASK_BYTES];
static atomic_t mask_update_seen;
static volatile uint16_t peer_table[ESB_BEACON_PEER_COUNT];

#define PEER_RSSI_SHIFT 8

static uint16_t peer_pack(uint8_t battery, int8_t rssi_dbm) {
    return (uint16_t)(battery | ((uint8_t)rssi_dbm << PEER_RSSI_SHIFT));
}

static uint8_t peer_unpack_battery(uint16_t entry) {
    return (uint8_t)entry;
}

static int8_t peer_unpack_rssi_dbm(uint16_t entry) {
    return (int8_t)(entry >> PEER_RSSI_SHIFT);
}

static void ensure_mask(void) {
    if (mask_ready) {
        return;
    }
    for (size_t channel = 0; channel < HOP_COUNT; channel++) {
        hop_policy_mask_set(active_mask, channel, true);
    }
    mask_ready = true;
}

#define HOP_RETAINED_MAGIC 0x484F5031

struct hop_retained {
    uint32_t magic;
    uint8_t hop_index;
    uint8_t epoch;
    uint8_t mask[ESB_HOP_MASK_BYTES];
    uint8_t checksum;
};
static __noinit struct hop_retained retained_link;

static uint8_t retained_checksum(const struct hop_retained *retained) {
    uint8_t sum = (uint8_t)(retained->hop_index ^ retained->epoch);
    for (size_t byte = 0; byte < ESB_HOP_MASK_BYTES; byte++) {
        sum ^= retained->mask[byte];
    }
    return sum;
}

static void retain_link_state(void) {
    retained_link.hop_index = hop_index;
    retained_link.epoch = adopted_epoch;
    memcpy(retained_link.mask, active_mask, ESB_HOP_MASK_BYTES);
    retained_link.checksum = retained_checksum(&retained_link);
    retained_link.magic = HOP_RETAINED_MAGIC;
}

void hop_restore(void) {
    if (HOP_COUNT <= 1) {
        return;
    }
    if (retained_link.magic != HOP_RETAINED_MAGIC ||
        retained_link.checksum != retained_checksum(&retained_link) ||
        retained_link.hop_index >= HOP_COUNT) {
        return;
    }
    ensure_mask();
    memcpy(active_mask, retained_link.mask, ESB_HOP_MASK_BYTES);
    adopted_epoch = retained_link.epoch;
    atomic_set(&beacon_epoch, adopted_epoch);
    hop_index = retained_link.hop_index;
}

/* Read under the lock the radio ISR stages with. */
static void adopt_staged_mask(void) {
    if (atomic_get(&mask_update_seen) == 0) {
        return;
    }
    unsigned int key = irq_lock();
    memcpy(active_mask, staged_mask, ESB_HOP_MASK_BYTES);
    irq_unlock(key);
}

/* Adopt the central's channel on a beacon epoch or mask change.
 * Otherwise sweep the pool to land on a stable central, then camp a hopping one.
 * The full pool is the rendezvous, so a stale mask still recovers.
 * Statically initialized for the same SYS_INIT-order reason as the central work. */
static void keepalive_work_fn(struct k_work *work);
static struct k_work_delayable keepalive_work = Z_WORK_DELAYABLE_INITIALIZER(keepalive_work_fn);
static void adopt_epoch(uint8_t epoch) {
    adopted_epoch = epoch;
    adopt_staged_mask(); /* swap mask with the epoch, matching the central's commit */
    hop_index = hop_policy_channel_for_epoch_masked(epoch, active_mask, HOP_COUNT);
    apply_hop_channel();
    bad_windows = 0;
    lost_windows = 0;
    camp_dwell = 0;
    degrade_undo_armed = false;
    atomic_set(&max_tx_attempts, 0);
}

static void connected_window(void) {
    lost_windows = 0;
    degrade_undo_armed = false;
    uint8_t attempts = (uint8_t)atomic_set(&max_tx_attempts, 0);
    if (attempts > 0) {
        attempts_ewma_x10 = hop_policy_ewma_update(attempts_ewma_x10, attempts);
        uint8_t budget = hop_policy_adaptive_retransmits(
            attempts_ewma_x10, ADAPTIVE_RETRANSMITS_MIN, retransmit_ceiling);
        if (budget != applied_retransmits && esb_set_retransmit_count(budget) == 0) {
            applied_retransmits = budget;
        }
    }
    uint8_t penalty = hop_policy_attempts_penalty(attempts, HOP_POLICY_GOOD_TX_ATTEMPTS);
    if (hop_policy_should_hop(&bad_windows, penalty, hop_threshold)) {
        degrade_undo_index = hop_index;
        degrade_undo_armed = true;
        hop_index = hop_policy_index_next_active(hop_index, active_mask, HOP_COUNT);
        apply_hop_channel();
    }
}

static void lost_window(void) {
    atomic_set(&max_tx_attempts, 0);
    bad_windows = 0;
    if (lost_windows < UINT16_MAX) {
        lost_windows++;
    }
    if (degrade_undo_armed) {
        degrade_undo_armed = false;
        hop_index = degrade_undo_index;
        apply_hop_channel();
    } else if (lost_windows < ESB_HOP_SWEEP_WINDOWS) {
        if (lost_windows % ESB_HOP_SWEEP_DWELL_WINDOWS == 0) {
            hop_index = hop_policy_index_next(hop_index, HOP_COUNT);
            apply_hop_channel();
        }
    } else {
        hop_policy_camp_step(&camp_anchor, &camp_dwell, ESB_HOP_ANCHOR_COUNT,
                             ESB_HOP_ANCHOR_DWELL_WINDOWS);
        uint8_t anchor_index = hop_anchor_index_at(camp_anchor);
        if (hop_index != anchor_index) {
            hop_index = anchor_index;
            apply_hop_channel();
        }
    }
}

static void keepalive_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (HOP_COUNT > 1) {
        ensure_mask();
        uint8_t epoch = (uint8_t)atomic_get(&beacon_epoch);
        if (epoch != adopted_epoch) {
            adopt_epoch(epoch);
        } else if (atomic_get(&link_acked) != 0) {
            connected_window();
        } else {
            lost_window();
        }
        if (atomic_get(&link_acked) != 0) {
            retain_link_state();
        }
    }
    bool active = atomic_set(&data_sent_since_tick, 0) != 0;
    bool searching = atomic_get(&link_acked) == 0;
    uint16_t period_ms = (active || searching) ? hop_window_ms : idle_keepalive_ms;
    esb_link_send_keepalive(active ? ESB_KEEPALIVE_ACTIVE : ESB_KEEPALIVE_IDLE);
    k_work_reschedule(&keepalive_work, K_MSEC(period_ms));
}

void hop_start(void) {
    k_work_reschedule(&keepalive_work, K_MSEC(hop_window_ms));
}

void hop_stop(void) {
    k_work_cancel_delayable(&keepalive_work);
}

bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi) {
    ARG_UNUSED(rssi);
    /* Fixed link beacons HID state too. */
    if (esb_is_beacon(data, length)) {
        const struct esb_beacon *beacon = (const struct esb_beacon *)data;
        atomic_set(&beacon_epoch, beacon->epoch); /* adopted in keepalive_work, not queued */
        if (pipe < ESB_BEACON_PEER_COUNT) {
            uplink_rssi_dbm = beacon->peers[pipe].rssi_dbm;
        }
        esb_link_hid_state_store(beacon->hid_modifiers, beacon->hid_indicators);
        for (uint8_t peer = 0; peer < ESB_BEACON_PEER_COUNT; peer++) {
            peer_table[peer] = peer_pack(beacon->peers[peer].battery,
                                         beacon->peers[peer].rssi_dbm);
        }
        return true;
    }
    if (HOP_COUNT <= 1) {
        return false;
    }
    if (esb_is_mask_update(data, length)) {
        const struct esb_mask_update *update = (const struct esb_mask_update *)data;
        memcpy(staged_mask, update->mask, ESB_HOP_MASK_BYTES);
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
    status->attempts_ewma_x10 = attempts_ewma_x10;
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

uint8_t zmk_split_esb_peer_battery(uint8_t pipe) {
    if (pipe >= ESB_BEACON_PEER_COUNT) {
        return ESB_KEEPALIVE_BATTERY_UNKNOWN;
    }
    uint16_t entry = peer_table[pipe];
    return peer_unpack_battery(entry);
}

int8_t zmk_split_esb_peer_rssi_dbm(uint8_t pipe) {
    if (pipe >= ESB_BEACON_PEER_COUNT) {
        return 0;
    }
    uint16_t entry = peer_table[pipe];
    return peer_unpack_rssi_dbm(entry);
}
