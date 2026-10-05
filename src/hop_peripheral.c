// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Peripheral hop engine: adopt the central's epoch and mask, sweep to re-find it on a bad uplink.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include "esb_keepalive.h"
#include "esb_link.h"
#include "spsc_latch.h"

#ifndef CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS
#define CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS 32
#endif
#include "hop.h"
#include "hop_internal.h"
#include "hop_policy.h"
#include "peripheral.h"
#include "wire_relay.h"

static const uint16_t hop_threshold = DT_INST_PROP(0, hop_threshold);
BUILD_ASSERT(DT_INST_PROP(0, hop_threshold) <= UINT8_MAX,
             "hop-threshold above 255 never fires, sweep streak saturates at UINT8_MAX");

static const uint8_t self_pipe = DT_PROP(DT_CHOSEN(zmk_esb_self), pipe);
BUILD_ASSERT(DT_PROP(DT_CHOSEN(zmk_esb_self), pipe) < ESB_BEACON_PEER_COUNT,
             "self pipe outside beacon peer table");

#define ADAPTIVE_RETRANSMITS_MIN 2
#define RETRANSMIT_CEILING_MAX DT_INST_PROP(0, retransmit_count)
BUILD_ASSERT(RETRANSMIT_CEILING_MAX <= UINT8_MAX, "retransmit-count above 255");
static atomic_t retransmit_ceiling = ATOMIC_INIT(RETRANSMIT_CEILING_MAX);
static uint16_t attempts_ewma_x10 = 10;
static const uint16_t hop_window_ms = DT_INST_PROP(0, hop_window_ms);
static const uint16_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);
static atomic_t max_tx_attempts;
static atomic_t data_sent_since_tick;
static atomic_t acked_data_since_tick;
static atomic_t ack_probe_done_since_tick;
static atomic_t tx_succeeded_since_tick;
static atomic_t tx_failed_since_tick;
static atomic_t link_acked;
static atomic_t beacon_epoch;
static uint8_t bad_windows;
static uint16_t lost_windows;
static uint8_t camp_anchor = ESB_HOP_ANCHOR_COUNT - 1;
static uint16_t camp_dwell;
static uint8_t degrade_undo_index;
static bool degrade_undo_armed;
static uint8_t adopted_epoch;
static atomic_t uplink_rssi_dbm;
static uint8_t active_mask[ESB_HOP_MASK_BYTES];
static bool mask_ready;

#define STAGED_MASK_SLOTS 2
struct staged_mask {
    uint8_t bytes[ESB_HOP_MASK_BYTES];
};
static struct staged_mask staged_mask_pool[STAGED_MASK_SLOTS];
static struct spsc_latch staged_mask_latch = {.slot_count = STAGED_MASK_SLOTS};
static atomic_t peer_table[ESB_BEACON_PEER_COUNT];

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

static uint16_t peer_entry_get(uint8_t pipe) {
    return (uint16_t)atomic_get(&peer_table[pipe]);
}

static int8_t uplink_rssi_dbm_get(void) {
    return (int8_t)atomic_get(&uplink_rssi_dbm);
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

static void adopt_staged_mask(void) {
    struct staged_mask *slot = spsc_latch_peek(&staged_mask_latch);
    if (slot == NULL) {
        return;
    }
    memcpy(active_mask, slot->bytes, ESB_HOP_MASK_BYTES);
    (void)spsc_latch_release(&staged_mask_latch, slot);
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
    attempts_ewma_x10 = 10;
    atomic_set(&max_tx_attempts, 0);
}

static void connected_window(void) {
    lost_windows = 0;
    degrade_undo_armed = false;
    uint8_t attempts = (uint8_t)atomic_set(&max_tx_attempts, 0);
    if (attempts > 0) {
        attempts_ewma_x10 = hop_policy_ewma_update(attempts_ewma_x10, attempts);
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

static uint8_t retransmit_budget(void) {
    uint8_t ceiling = (uint8_t)atomic_get(&retransmit_ceiling);
    if (HOP_COUNT <= 1) {
        return ceiling;
    }
    uint8_t retransmit_floor = MIN(ADAPTIVE_RETRANSMITS_MIN, ceiling);
    return hop_policy_adaptive_retransmits(attempts_ewma_x10, retransmit_floor, ceiling);
}

/* Fire-and-forget sends raise TX success too, so only a failure proves loss. */
static void settle_link_state(void) {
    bool failed = atomic_set(&tx_failed_since_tick, 0) != 0;
    bool succeeded = atomic_set(&tx_succeeded_since_tick, 0) != 0;
    if (failed) {
        atomic_set(&link_acked, 0);
    } else if (succeeded) {
        atomic_set(&link_acked, 1);
    }
}

static void keepalive_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    settle_link_state();
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
    esb_link_set_retransmit_count(retransmit_budget());
    esb_link_apply_pending();
    bool active = atomic_set(&data_sent_since_tick, 0) != 0;
    bool acked_data = atomic_set(&acked_data_since_tick, 0) != 0;
    atomic_set(&ack_probe_done_since_tick, 0);
    bool searching = atomic_get(&link_acked) == 0;
    bool force_fast = DT_ENUM_HAS_VALUE(DT_CHOSEN(zmk_esb_self), role, relay);
    uint16_t period_ms;
    if (force_fast) {
        period_ms = CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS;
    } else {
        period_ms = (active || searching) ? hop_window_ms : idle_keepalive_ms;
    }
    if (!acked_data || searching) {
        esb_link_send_keepalive(esb_keepalive_peripheral_state(active, searching));
        /* Probe for this window only, counted as acked data it would skip the next keepalive. */
        atomic_set(&ack_probe_done_since_tick, 1);
    }
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
        atomic_set(&uplink_rssi_dbm, beacon->peers[self_pipe].rssi_dbm);
        peripheral_hid_state_store(beacon->hid_modifiers, beacon->hid_indicators);
        for (uint8_t peer = 0; peer < ESB_BEACON_PEER_COUNT; peer++) {
            atomic_set(&peer_table[peer], peer_pack(beacon->peers[peer].battery,
                                                    beacon->peers[peer].rssi_dbm));
        }
        return !wire_relay_owns_pipe(pipe);
    }
    if (HOP_COUNT <= 1) {
        return false;
    }
    if (esb_is_mask_update(data, length)) {
        const struct esb_mask_update *update = (const struct esb_mask_update *)data;
        uint8_t index = spsc_latch_claim(&staged_mask_latch);
        memcpy(staged_mask_pool[index].bytes, update->mask, ESB_HOP_MASK_BYTES);
        spsc_latch_publish(&staged_mask_latch, &staged_mask_pool[index]);
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
    atomic_set(&tx_succeeded_since_tick, 1);
    if (HOP_COUNT > 1) {
        record_tx_attempts(attempts);
    }
}

void hop_note_tx_failed(void) {
    atomic_set(&tx_failed_since_tick, 1);
    if (HOP_COUNT > 1) {
        record_tx_attempts(0xFF); /* a lost packet is the worst this window */
    }
}

void hop_note_data_sent(bool acked) {
    atomic_set(&data_sent_since_tick, 1);
    if (acked) {
        atomic_set(&acked_data_since_tick, 1);
        atomic_set(&ack_probe_done_since_tick, 1);
    }
}

bool hop_ack_probe_due(void) {
    return atomic_get(&ack_probe_done_since_tick) == 0;
}

int hop_set_retransmit_ceiling(uint32_t ceiling) {
    if (ceiling > RETRANSMIT_CEILING_MAX) {
        return -EINVAL;
    }
    atomic_set(&retransmit_ceiling, (atomic_val_t)ceiling);
    return 0;
}

uint8_t hop_link_cost_x10(void) {
    return (uint8_t)MIN(attempts_ewma_x10, UINT8_MAX);
}

void zmk_split_esb_get_status(struct zmk_split_esb_status *status) {
    __ASSERT_NO_MSG(status != NULL);
    status->channel = hop_current_channel();
    status->epoch = adopted_epoch;
    status->searching = atomic_get(&link_acked) == 0;
    status->rssi_dbm = uplink_rssi_dbm_get();
    status->attempts_ewma_x10 = attempts_ewma_x10;
}

uint8_t zmk_split_esb_pipe_count(void) {
    return 1;
}

int8_t zmk_split_esb_pipe_rssi_dbm(uint8_t pipe) {
    if (pipe >= 1) {
        return 0;
    }
    return uplink_rssi_dbm_get();
}

uint8_t zmk_split_esb_peer_battery(uint8_t pipe) {
    if (pipe >= ESB_BEACON_PEER_COUNT) {
        return ESB_KEEPALIVE_BATTERY_UNKNOWN;
    }
    return peer_unpack_battery(peer_entry_get(pipe));
}

int8_t zmk_split_esb_peer_rssi_dbm(uint8_t pipe) {
    if (pipe >= ESB_BEACON_PEER_COUNT) {
        return 0;
    }
    return peer_unpack_rssi_dbm(peer_entry_get(pipe));
}
