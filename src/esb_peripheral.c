// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* ESB radio transport for a split peripheral. */
#define DT_DRV_COMPAT zmk_split_esb

#include "peripheral.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include "esb_batch.h"
#include "esb_hid_relay_peripheral.h"
#include "esb_link.h"
#include "esb_wire.h"
#include "hop.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

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

static struct esb_batch batch;

uint8_t esb_link_keepalive_fill(uint8_t *out, size_t out_size, uint8_t state) {
    return peripheral_keepalive_fill(out, out_size, state, hop_link_cost_x10());
}

static int esb_peripheral_report_event(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT) {
        return esb_batch_report_event(&batch, event, event_wants_ack(event));
    }
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    int encoded = peripheral_encode_event(event, wire, sizeof(wire));
    if (encoded < 0) {
        return encoded;
    }
    return esb_link_send(wire, (size_t)encoded, event_wants_ack(event));
}

static int esb_peripheral_set_enabled(bool enabled) {
    transport_enabled = enabled;
    return esb_link_set_enabled(enabled);
}

static struct zmk_split_transport_status esb_peripheral_get_status(void) {
    return (struct zmk_split_transport_status){
        .available = true,
        .enabled = transport_enabled,
        /* ESB is connectionless: no connection state to track, so always connected. */
        .connections = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED,
    };
}

static int
esb_peripheral_set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t callback) {
    ARG_UNUSED(callback);
    return 0;
}

static const struct zmk_split_transport_peripheral_api esb_peripheral_api = {
    .report_event = esb_peripheral_report_event,
    .set_enabled = esb_peripheral_set_enabled,
    .get_status = esb_peripheral_get_status,
    .set_status_callback = esb_peripheral_set_status_callback,
};

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(esb_peripheral, &esb_peripheral_api,
                                        CONFIG_ZMK_SPLIT_ESB_PRIORITY);

#define SELF_IS_RELAY DT_ENUM_HAS_VALUE(DT_CHOSEN(zmk_esb_self), role, relay)

static void esb_peripheral_on_rx(uint8_t pipe, const uint8_t *data, size_t length) {
    ARG_UNUSED(pipe);
    if (SELF_IS_RELAY) {
        esb_hid_relay_deliver(data, length);
    } else {
        peripheral_deliver_command(data, length);
    }
}

static int esb_peripheral_init(void) {
    peripheral_set_transport(&esb_peripheral);
    hop_restore();
    return esb_link_init(esb_peripheral_on_rx);
}

SYS_INIT(esb_peripheral_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
