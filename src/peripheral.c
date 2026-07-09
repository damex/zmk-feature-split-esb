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

#include <zmk/battery.h>
#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include <zmk_split_esb.h>

#include "esb_batch.h"
#include "esb_hid_state.h"
#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_wire.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

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

static struct esb_batch batch;

static uint8_t pressed_positions[ESB_KEEPALIVE_BITMAP_BYTES];

const uint8_t *esb_link_keepalive_bitmap(void) {
    return pressed_positions;
}

uint8_t esb_link_keepalive_battery_level(void) {
    if (!IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)) {
        return ESB_KEEPALIVE_BATTERY_UNKNOWN;
    }
    uint8_t level = zmk_battery_state_of_charge();
    /* Zero is pre-first-sample, not 0% charge. */
    return (level == 0) ? ESB_KEEPALIVE_BATTERY_UNKNOWN : level;
}

static int peripheral_report_event(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT) {
        return esb_batch_report_event(&batch, event, event_wants_ack(event));
    }
    if (event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        /* Bitmap records intent even when the send fails below:
         * keepalive reconcile heals the miss. */
        esb_keepalive_bitmap_set(pressed_positions, event->data.key_position_event.position,
                                 event->data.key_position_event.pressed);
    }
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    size_t length = esb_wire_encode_event(wire, sizeof(wire), event);
    return esb_link_send(wire, length, event_wants_ack(event));
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

/* Commands invoke behaviors, same single-context contract as central events. */
K_MSGQ_DEFINE(peripheral_command_msgq, sizeof(struct zmk_split_transport_central_command),
              CONFIG_ZMK_SPLIT_ESB_COMMAND_QUEUE_SIZE, 4);

static void peripheral_command_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct zmk_split_transport_central_command command;
    while (k_msgq_get(&peripheral_command_msgq, &command, K_NO_WAIT) == 0) {
        zmk_split_transport_peripheral_command_handler(&esb_peripheral, command);
    }
}

static K_WORK_DEFINE(peripheral_command_work, peripheral_command_work_fn);

/* Single store, else reader mixes modifiers and indicators from two beacons. */
static atomic_t synced_hid_state;

uint8_t zmk_split_esb_hid_modifiers(void) {
    return esb_hid_state_modifiers((uint16_t)atomic_get(&synced_hid_state));
}

uint8_t zmk_split_esb_hid_indicators(void) {
    return esb_hid_state_indicators((uint16_t)atomic_get(&synced_hid_state));
}

void esb_link_hid_state_store(uint8_t modifiers, uint8_t indicators) {
    atomic_set(&synced_hid_state, esb_hid_state_pack(modifiers, indicators));
}

static void peripheral_on_rx(uint8_t pipe, const uint8_t *data, size_t length) {
    ARG_UNUSED(pipe);
    if (length != sizeof(struct zmk_split_transport_central_command)) {
        LOG_WRN("Dropping command with unexpected size %u", (unsigned int)length);
        LOG_HEXDUMP_DBG(data, length, "unexpected command payload");
        return;
    }
    struct zmk_split_transport_central_command command;
    memcpy(&command, data, sizeof(command));
    if (k_msgq_put(&peripheral_command_msgq, &command, K_NO_WAIT) < 0) {
        LOG_WRN("Dropping command, queue full");
        return;
    }
    k_work_submit(&peripheral_command_work);
}

static int peripheral_init(void) {
    return esb_link_init(peripheral_on_rx);
}

SYS_INIT(peripheral_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
