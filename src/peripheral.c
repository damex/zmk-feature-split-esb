// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * ZMK ESB split peripheral.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include "esb_link.h"
#include "esb_wire.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct zmk_split_transport_peripheral_event) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "peripheral event does not fit in one ESB payload; raise ZMK_SPLIT_ESB_MAX_PAYLOAD");
BUILD_ASSERT(sizeof(struct zmk_split_transport_central_command) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "central command does not fit in one ESB payload; raise ZMK_SPLIT_ESB_MAX_PAYLOAD");

static bool transport_enabled;

#if DT_INST_NODE_HAS_PROP(0, lossy_codes)

#define LOSSY_DT_CELL(node_id, prop, index) DT_PROP_BY_IDX(node_id, prop, index)

static const uint32_t lossy_dt_cells[] = {
    DT_INST_FOREACH_PROP_ELEM_SEP(0, lossy_codes, LOSSY_DT_CELL, (,))
};

BUILD_ASSERT((ARRAY_SIZE(lossy_dt_cells) % 2) == 0,
             "zmk,split-esb lossy-codes must be (type, code) pairs");

static bool input_is_lossy(uint8_t input_type, uint16_t input_code) {
    for (size_t pair = 0; pair < ARRAY_SIZE(lossy_dt_cells); pair += 2) {
        if ((uint8_t)lossy_dt_cells[pair] == input_type
            && (uint16_t)lossy_dt_cells[pair + 1] == input_code) {
            return true;
        }
    }
    return false;
}

#else

static bool input_is_lossy(uint8_t input_type, uint16_t input_code) {
    ARG_UNUSED(input_type);
    ARG_UNUSED(input_code);
    return false;
}

#endif /* DT_INST_NODE_HAS_PROP(0, lossy_codes) */

static bool event_wants_ack(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT) {
        return true;
    }
    return !input_is_lossy(event->data.input_event.type, event->data.input_event.code);
}

/* Coalesce one report's per-axis events into a single packet: ZMK forwards each
 * axis as its own event (REL_X sync=0, REL_Y sync=1), doubling on-air packets at high rate.
 * Buffer input events and flush on the sync event (the report boundary, us later, so no
 * real latency added).
 * Non-input events flush the batch and go alone.
 * Single context (the input thread), so no lock. */
#define PERIPHERAL_BATCH_MAX                                                                       \
    (CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD / sizeof(struct zmk_split_transport_peripheral_event))
BUILD_ASSERT(PERIPHERAL_BATCH_MAX >= 2,
             "ZMK_SPLIT_ESB_MAX_PAYLOAD too small to coalesce a 2-axis sample; raise it");

static struct zmk_split_transport_peripheral_event batch[PERIPHERAL_BATCH_MAX];
static size_t batch_count;
static bool batch_wants_ack;

static int peripheral_flush_batch(void) {
    if (batch_count == 0) {
        return 0;
    }
    uint8_t wire[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
    size_t length = 0;
    for (size_t index = 0; index < batch_count; index++) {
        size_t written = esb_wire_encode_event(&wire[length], sizeof(wire) - length, &batch[index]);
        if (written == 0) {
            break; /* next event would overflow the packet, send what fits */
        }
        length += written;
    }
    int error = esb_link_send(wire, length, batch_wants_ack);
    batch_count = 0;
    batch_wants_ack = false;
    return error;
}

static int peripheral_report_event(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT) {
        int flush_error = peripheral_flush_batch();
        if (flush_error < 0) {
            return flush_error;
        }
        uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
        size_t length = esb_wire_encode_event(wire, sizeof(wire), event);
        return esb_link_send(wire, length, event_wants_ack(event));
    }
    batch[batch_count] = *event;
    batch_count++;
    if (event_wants_ack(event)) {
        batch_wants_ack = true;
    }
    if (event->data.input_event.sync || batch_count >= PERIPHERAL_BATCH_MAX) {
        return peripheral_flush_batch();
    }
    return 0;
}

static int peripheral_set_enabled(bool enabled) {
    transport_enabled = enabled;
    return esb_link_set_enabled(enabled);
}

static struct zmk_split_transport_status peripheral_get_status(void) {
    return (struct zmk_split_transport_status){
        .available = true,
        .enabled = transport_enabled,
        /* ESB is connectionless: no connection state to track, so always connected. */
        .connections = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED,
    };
}

static int
peripheral_set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t callback) {
    ARG_UNUSED(callback);
    return 0;
}

static const struct zmk_split_transport_peripheral_api peripheral_api = {
    .report_event = peripheral_report_event,
    .set_enabled = peripheral_set_enabled,
    .get_status = peripheral_get_status,
    .set_status_callback = peripheral_set_status_callback,
};

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(esb_peripheral, &peripheral_api, CONFIG_ZMK_SPLIT_ESB_PRIORITY);

/* Runs in the esb_link dispatch thread (not the radio ISR). */
static void peripheral_on_rx(uint8_t pipe, const uint8_t *data, size_t length) {
    ARG_UNUSED(pipe);
    if (length != sizeof(struct zmk_split_transport_central_command)) {
        LOG_WRN("Dropping command with unexpected size %u", (unsigned int)length);
        return;
    }
    struct zmk_split_transport_central_command command;
    memcpy(&command, data, sizeof(command));
    zmk_split_transport_peripheral_command_handler(&esb_peripheral, command);
}

static int peripheral_init(void) {
    return esb_link_init(peripheral_on_rx);
}

SYS_INIT(peripheral_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
