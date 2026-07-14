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
#include <zephyr/sys/time_units.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_esb_self), "peripheral needs a chosen zmk,esb-self");
static const uint8_t self_pipe = DT_PROP(DT_CHOSEN(zmk_esb_self), pipe);

/* Worst legitimate completion-event silence is one packet exhausting its
 * retransmits. TX_STALL_MARGIN such cycles with the FIFO still full means the
 * engine stalled; the floor covers per-attempt airtime the product omits. */
#define TX_STALL_MARGIN 4
#define TX_STALL_FLOOR_MS 100
#define TX_STALL_TIMEOUT_MS                                                                        \
    MAX(TX_STALL_FLOOR_MS, (TX_STALL_MARGIN * DT_INST_PROP(0, retransmit_count) *                  \
                            DT_INST_PROP(0, retransmit_delay_us)) /                                \
                               USEC_PER_MSEC)

/* PTX needs HFXO only around TX bursts. */
#define HFCLK_IDLE_HOLD_MARGIN 2
#define HFCLK_IDLE_HOLD_FLOOR_MS 10
#define HFCLK_IDLE_HOLD_MS                                                                         \
    MAX(HFCLK_IDLE_HOLD_FLOOR_MS,                                                                  \
        (HFCLK_IDLE_HOLD_MARGIN * DT_INST_PROP(0, retransmit_count) *                              \
         DT_INST_PROP(0, retransmit_delay_us)) /                                                   \
            USEC_PER_MSEC)

static K_MUTEX_DEFINE(hfclk_gate_mutex);
static bool hfclk_gating;

static void hfclk_release_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&hfclk_gate_mutex, K_FOREVER);
    /* Cancel misses an already-running item, so recheck under the lock. */
    if (hfclk_gating) {
        esb_link_hfclk_release();
    }
    k_mutex_unlock(&hfclk_gate_mutex);
}

static K_WORK_DELAYABLE_DEFINE(hfclk_release_work, hfclk_release_work_fn);

static void hfclk_gate_hold(void) {
    k_mutex_lock(&hfclk_gate_mutex, K_FOREVER);
    (void)esb_link_hfclk_acquire();
    if (hfclk_gating) {
        k_work_reschedule(&hfclk_release_work, K_MSEC(HFCLK_IDLE_HOLD_MS));
    }
    k_mutex_unlock(&hfclk_gate_mutex);
}

void esb_link_set_idle(bool idle) {
    k_mutex_lock(&hfclk_gate_mutex, K_FOREVER);
    hfclk_gating = idle;
    if (idle) {
        k_work_reschedule(&hfclk_release_work, K_MSEC(HFCLK_IDLE_HOLD_MS));
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
    if (error == -ENOMEM &&
        (k_uptime_get_32() - esb_link_tx_last_event_ms()) > TX_STALL_TIMEOUT_MS) {
        LOG_WRN("TX engine stalled, flushing to recover");
        (void)esb_flush_tx();
        esb_link_mark_tx_event();
        error = esb_write_payload(payload);
    }
    irq_unlock(key);
    return error;
}

int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    if (length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    hop_note_data_sent();
    struct esb_payload payload = {0};
    payload.pipe = self_pipe;
    payload.noack = !ack;
    payload.length = (uint8_t)length;
    memcpy(payload.data, data, length);
    int error = submit_payload(&payload);
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

int esb_link_role_start(void) {
    return 0;
}

void esb_link_role_rx_done(uint8_t pipes_seen) {
    ARG_UNUSED(pipes_seen);
}
