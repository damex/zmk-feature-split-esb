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

BUILD_ASSERT(ESB_KEEPALIVE_LENGTH <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "keepalive does not fit in one ESB payload");

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

/* esb_write_payload checks FIFO space before its internal irq_lock, so two
 * submitters racing at a near-full FIFO can overflow the ring. Input thread and
 * system workqueue both submit here: extend esb.c's own lock domain over the
 * unlocked pre-check. */
static int submit_payload(const struct esb_payload *payload) {
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

void esb_link_send_keepalive(uint8_t state, const uint8_t *position_bitmap,
                             uint8_t battery_level) {
    struct esb_payload keepalive = {0};
    keepalive.pipe = self_pipe;
    keepalive.length = ESB_KEEPALIVE_LENGTH;
    esb_keepalive_encode(keepalive.data, state, position_bitmap, battery_level);
    (void)submit_payload(&keepalive);
}

int esb_link_role_start(void) {
    return 0;
}

void esb_link_role_rx_done(void) {
}
