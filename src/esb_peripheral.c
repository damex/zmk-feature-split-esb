// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* ESB radio transport for a split peripheral. */
#define DT_DRV_COMPAT zmk_split_esb

#include "peripheral.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include "esb_batch.h"
#include "esb_hid_relay_peripheral.h"
#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_motion_total.h"
#include "esb_wire.h"
#include "hop.h"
#include "peripheral_input.h"
#include "wire_relay.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static bool transport_enabled;

#if DT_INST_NODE_HAS_PROP(0, lossy_codes)

#define LOSSY_DT_CELL(node_id, prop, index) DT_PROP_BY_IDX(node_id, prop, index)

static const uint32_t lossy_dt_cells[] = {
    DT_INST_FOREACH_PROP_ELEM_SEP(0, lossy_codes, LOSSY_DT_CELL, (,))
};

BUILD_ASSERT((ARRAY_SIZE(lossy_dt_cells) % PERIPHERAL_LOSSY_PAIR_CELLS) == 0,
             "zmk,split-esb lossy-codes must be (type, code) pairs");

static bool input_is_lossy(uint8_t input_type, uint16_t input_code) {
    for (size_t pair = 0; pair < ARRAY_SIZE(lossy_dt_cells); pair += PERIPHERAL_LOSSY_PAIR_CELLS) {
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

#define MOTION_SLOT_COUNT MAX(PERIPHERAL_MOTION_TOTALS_MAX, 1)

/* Input thread claims and adds, the keepalive tick reads. */
struct motion_slot {
    atomic_t claimed;
    uint8_t reg;
    uint8_t code;
    atomic_t total;
};

static struct motion_slot motion_slots[MOTION_SLOT_COUNT];

static struct motion_slot *motion_slot_for(uint8_t reg, uint8_t code) {
    for (size_t index = 0; index < ARRAY_SIZE(motion_slots); index++) {
        struct motion_slot *slot = &motion_slots[index];
        if (atomic_get(&slot->claimed) == 0) {
            slot->reg = reg;
            slot->code = code;
            atomic_set(&slot->claimed, 1);
            return slot;
        }
        if (slot->reg == reg && slot->code == code) {
            return slot;
        }
    }
    return NULL;
}

static void motion_to_total(struct zmk_split_transport_peripheral_event *event) {
    if (event->data.input_event.type != INPUT_EV_REL || event->data.input_event.code > UINT8_MAX) {
        return;
    }
    struct motion_slot *slot =
        motion_slot_for(event->data.input_event.reg, (uint8_t)event->data.input_event.code);
    if (slot == NULL) {
        return;
    }
    int32_t total =
        esb_motion_total_add((int32_t)atomic_get(&slot->total), event->data.input_event.value);
    atomic_set(&slot->total, total);
    event->data.input_event.value = total;
    event->data.input_event.sync = (uint8_t)(event->data.input_event.sync | ESB_WIRE_INPUT_TOTAL);
}

static uint8_t motion_totals_snapshot(struct esb_keepalive_motion_total *out) {
    uint8_t count = 0;
    for (size_t index = 0; index < ARRAY_SIZE(motion_slots); index++) {
        const struct motion_slot *slot = &motion_slots[index];
        if (atomic_get(&slot->claimed) == 0) {
            continue;
        }
        out[count] = (struct esb_keepalive_motion_total){
            .reg = slot->reg,
            .code = slot->code,
            .short_total = (int16_t)(uint16_t)atomic_get(&slot->total),
        };
        count++;
    }
    return count;
}

static bool event_wants_ack(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT) {
        return true;
    }
    return !input_is_lossy(event->data.input_event.type, event->data.input_event.code);
}

static struct esb_batch batch;

uint8_t esb_link_keepalive_fill(uint8_t *out, size_t out_size, uint8_t state) {
    struct esb_keepalive_motion_total motion_totals[MOTION_SLOT_COUNT];
    uint8_t motion_count = motion_totals_snapshot(motion_totals);
    return peripheral_keepalive_fill(out, out_size, state, hop_link_cost_x10(), motion_totals,
                                     motion_count);
}

static int esb_peripheral_report_event(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT) {
        peripheral_input_note_event(event);
        bool wants_ack = event_wants_ack(event);
        struct zmk_split_transport_peripheral_event sent = *event;
        if (!wants_ack) {
            motion_to_total(&sent);
        }
        return esb_batch_report_event(&batch, &sent, wants_ack);
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
    if (wire_relay_owns_pipe(pipe)) {
        wire_relay_forward_to_peer(data, length);
        return;
    }
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
