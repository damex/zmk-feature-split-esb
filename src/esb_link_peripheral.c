// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Peripheral half of the ESB radio layer: uplink send and keepalive.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "esb_hid_state.h"
#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"
#include "hop_policy.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_esb_self), "peripheral needs a chosen zmk,esb-self");
static const uint8_t self_pipe = DT_PROP(DT_CHOSEN(zmk_esb_self), pipe);

struct radio_setting {
    atomic_t requested;
    atomic_t applied;
    atomic_t pending;
    int (*apply)(atomic_val_t value);
};

static int apply_tx_power(atomic_val_t value) {
    return esb_set_tx_power((int8_t)value);
}

static int apply_retransmit_count(atomic_val_t value) {
    return esb_set_retransmit_count((uint16_t)value);
}

static int apply_retransmit_delay(atomic_val_t value) {
    return esb_set_retransmit_delay((uint16_t)value);
}

static struct radio_setting tx_power_setting = {
    .requested = ATOMIC_INIT(DT_INST_PROP(0, tx_power_dbm)),
    .applied = ATOMIC_INIT(DT_INST_PROP(0, tx_power_dbm)),
    .apply = apply_tx_power,
};

static struct radio_setting retransmit_count_setting = {
    .requested = ATOMIC_INIT(DT_INST_PROP(0, retransmit_count)),
    .applied = ATOMIC_INIT(DT_INST_PROP(0, retransmit_count)),
    .apply = apply_retransmit_count,
};

static struct radio_setting retransmit_delay_setting = {
    .requested = ATOMIC_INIT(DT_INST_PROP(0, retransmit_delay_us)),
    .applied = ATOMIC_INIT(DT_INST_PROP(0, retransmit_delay_us)),
    .apply = apply_retransmit_delay,
};

/* Radio refuses setting changes outside idle, so a busy radio keeps the value pending. */
static int apply_setting(struct radio_setting *setting) {
    if (!atomic_cas(&setting->pending, 1, 0)) {
        return 0;
    }
    atomic_val_t value = atomic_get(&setting->requested);
    int error = setting->apply(value);
    if (error == -EBUSY) {
        atomic_set(&setting->pending, 1);
        return 0;
    }
    if (error == 0) {
        atomic_set(&setting->applied, value);
    }
    return error;
}

static int request_setting(struct radio_setting *setting, atomic_val_t value) {
    atomic_set(&setting->requested, value);
    atomic_set(&setting->pending, 1);
    return apply_setting(setting);
}

int esb_link_set_tx_power(int32_t dbm) {
    if (dbm < INT8_MIN || dbm > INT8_MAX) {
        return -EINVAL;
    }
    return request_setting(&tx_power_setting, dbm);
}

int esb_link_set_retransmit_delay(uint32_t delay_us) {
    if (delay_us > UINT16_MAX) {
        return -EINVAL;
    }
    return request_setting(&retransmit_delay_setting, (atomic_val_t)delay_us);
}

void esb_link_set_retransmit_count(uint8_t count) {
    (void)request_setting(&retransmit_count_setting, count);
}

void esb_link_apply_pending(void) {
    struct radio_setting *const settings[] = {
        &tx_power_setting,
        &retransmit_count_setting,
        &retransmit_delay_setting,
    };
    for (size_t index = 0; index < ARRAY_SIZE(settings); index++) {
        int error = apply_setting(settings[index]);
        if (error < 0) {
            LOG_WRN("radio setting rejected (%d)", error);
        }
    }
}

/* Worst legitimate completion-event silence is one packet exhausting its
 * retransmits. TX_STALL_MARGIN such cycles with the FIFO still full means the
 * engine stalled; the floor covers per-attempt airtime the product omits. */
#define TX_STALL_MARGIN 4
#define TX_STALL_FLOOR_MS 100

/* PTX needs HFXO only around TX bursts. */
#define HFCLK_IDLE_HOLD_MARGIN 2
#define HFCLK_IDLE_HOLD_FLOOR_MS 10

static uint32_t live_retry_cycle_ms(uint8_t margin, uint32_t floor_ms) {
    uint8_t count = (uint8_t)atomic_get(&retransmit_count_setting.applied);
    uint16_t delay_us = (uint16_t)atomic_get(&retransmit_delay_setting.applied);
    return hop_policy_retry_cycle_ms(count, delay_us, margin, floor_ms);
}

static K_MUTEX_DEFINE(hfclk_gate_mutex);
static bool hfclk_gating;

static void hfclk_release_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hfclk_release_work, hfclk_release_work_fn);

static void hfclk_release_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&hfclk_gate_mutex, K_FOREVER);
    /* Cancel misses an already-running item, so recheck under the lock. */
    if (hop_policy_hfclk_release_allowed(hfclk_gating, esb_is_idle())) {
        esb_link_hfclk_release();
    } else if (hfclk_gating) {
        k_work_reschedule(&hfclk_release_work,
                          K_MSEC(live_retry_cycle_ms(HFCLK_IDLE_HOLD_MARGIN,
                                                     HFCLK_IDLE_HOLD_FLOOR_MS)));
    }
    k_mutex_unlock(&hfclk_gate_mutex);
}

static void hfclk_gate_hold(void) {
    k_mutex_lock(&hfclk_gate_mutex, K_FOREVER);
    (void)esb_link_hfclk_acquire();
    if (hfclk_gating) {
        k_work_reschedule(&hfclk_release_work,
                          K_MSEC(live_retry_cycle_ms(HFCLK_IDLE_HOLD_MARGIN,
                                                     HFCLK_IDLE_HOLD_FLOOR_MS)));
    }
    k_mutex_unlock(&hfclk_gate_mutex);
}

void esb_link_set_idle(bool idle) {
    k_mutex_lock(&hfclk_gate_mutex, K_FOREVER);
    hfclk_gating = idle;
    if (idle) {
        k_work_reschedule(&hfclk_release_work,
                          K_MSEC(live_retry_cycle_ms(HFCLK_IDLE_HOLD_MARGIN,
                                                     HFCLK_IDLE_HOLD_FLOOR_MS)));
    } else {
        k_work_cancel_delayable(&hfclk_release_work);
        (void)esb_link_hfclk_acquire();
    }
    k_mutex_unlock(&hfclk_gate_mutex);
}

/* esb_write_payload checks FIFO space before its internal irq_lock, so two
 * submitters racing at a near-full FIFO can overflow the ring. Input thread and
 * system workqueue both submit here: extend esb.c's own lock domain over the
 * unlocked pre-check. */
static int submit_payload(const struct esb_payload *payload) {
    /* Before irq_lock: the HFXO-ready spinwait needs the clock interrupt. */
    hfclk_gate_hold();
    unsigned int key = irq_lock();
    int error = esb_write_payload(payload);
    irq_unlock(key);
    uint32_t silence_ms = k_uptime_get_32() - esb_link_tx_last_event_ms();
    if (error != -ENOMEM || silence_ms <= live_retry_cycle_ms(TX_STALL_MARGIN, TX_STALL_FLOOR_MS)) {
        return error;
    }
    LOG_WRN("TX engine stalled, flushing to recover");
    (void)esb_flush_tx();
    esb_link_mark_tx_event();
    key = irq_lock();
    error = esb_write_payload(payload);
    irq_unlock(key);
    return error;
}

static atomic_t tx_restarts;

static void tx_restart_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    hfclk_gate_hold();
    /* Same lock as submit_payload, else a racing write starts the head twice. */
    unsigned int key = irq_lock();
    int error = esb_start_tx();
    irq_unlock(key);
    if (error) {
        LOG_DBG("esb_start_tx after TX_FAILED returned %d", error);
    }
}
static K_WORK_DEFINE(tx_restart_work, tx_restart_work_fn);

static int send_on_pipe(uint8_t pipe, const uint8_t *data, size_t length, bool ack) {
    if (length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    struct esb_payload payload = {0};
    payload.pipe = pipe;
    payload.noack = !ack;
    payload.length = (uint8_t)length;
    memcpy(payload.data, data, length);
    int error = submit_payload(&payload);
    if (error == 0) {
        hop_note_data_sent();
    }
    return error;
}

int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    int error = send_on_pipe(self_pipe, data, length, ack);
    if (error) {
        LOG_WRN("uplink event dropped, esb_write_payload returned %d", error);
    }
    return error;
}

void esb_link_send_keepalive(uint8_t state) {
    struct esb_payload keepalive = {0};
    keepalive.pipe = self_pipe;
    keepalive.length = esb_link_keepalive_fill(keepalive.data, sizeof(keepalive.data), state);
    if (keepalive.length == 0) {
        return;
    }
    (void)submit_payload(&keepalive);
}

void esb_link_send_relay_poll(void) {
    const struct esb_relay_poll poll = {.tag = ESB_RELAY_POLL_TAG};
    struct esb_payload payload = {0};
    payload.pipe = self_pipe;
    payload.length = sizeof(poll);
    memcpy(payload.data, &poll, sizeof(poll));
    (void)submit_payload(&payload);
}

#if defined(CONFIG_ZMK_SPLIT_ESB_WIRE_RELAY)
BUILD_ASSERT(DT_HAS_CHOSEN(zmk_esb_wire_peer),
             "wire relay needs a chosen zmk,esb-wire-peer");
static const uint8_t peer_pipe = DT_PROP(DT_CHOSEN(zmk_esb_wire_peer), pipe);
BUILD_ASSERT(DT_PROP(DT_CHOSEN(zmk_esb_wire_peer), pipe) !=
             DT_PROP(DT_CHOSEN(zmk_esb_self), pipe),
             "wire peer pipe must differ from self pipe");

int esb_link_send_relay(const uint8_t *data, size_t length, bool ack) {
    int error = send_on_pipe(peer_pipe, data, length, ack);
    if (error) {
        LOG_WRN("relay event dropped, esb_write_payload returned %d", error);
    }
    return error;
}
#endif

int esb_link_role_start(void) {
    return 0;
}

void esb_link_role_rx_done(uint8_t pipes_seen) {
    ARG_UNUSED(pipes_seen);
}

void esb_link_role_tx_succeeded(void) {
    atomic_set(&tx_restarts, 0);
}

bool esb_link_role_retry_failed_tx(void) {
    apply_hop_channel();
    atomic_val_t restarts = atomic_inc(&tx_restarts);
    if (!hop_link_acked() || restarts >= ESB_LINK_TX_RESTARTS_MAX) {
        atomic_set(&tx_restarts, 0);
        return false;
    }
    k_work_submit(&tx_restart_work);
    return true;
}

void esb_link_role_stop(void) {
    (void)k_work_cancel(&tx_restart_work);
    atomic_set(&tx_restarts, 0);
}
