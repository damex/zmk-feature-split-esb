// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * ZMK ESB split central. Source id = ESB pipe.
 */
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/events/battery_state_changed.h>
#include <zmk/split/transport/central.h>
#include <zmk/split/transport/types.h>

#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_wire.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct zmk_split_transport_peripheral_event) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "peripheral event does not fit in one ESB payload; raise ZMK_SPLIT_ESB_MAX_PAYLOAD");
BUILD_ASSERT(sizeof(struct zmk_split_transport_central_command) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "central command does not fit in one ESB payload; raise ZMK_SPLIT_ESB_MAX_PAYLOAD");

static bool transport_enabled;

static int central_send_command(uint8_t source,
                                struct zmk_split_transport_central_command command) {
    return esb_link_stage_reply(source, (const uint8_t *)&command, sizeof(command));
}

static int central_get_available_source_ids(uint8_t *sources) {
    return esb_link_source_ids(sources);
}

static int central_set_enabled(bool enabled) {
    transport_enabled = enabled;
    return esb_link_set_enabled(enabled);
}

static struct zmk_split_transport_status central_get_status(void) {
    return (struct zmk_split_transport_status){
        .available = true,
        .enabled = transport_enabled,
        /* ESB is connectionless: no connection state to track, so always connected. */
        .connections = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED,
    };
}

static int
central_set_status_callback(zmk_split_transport_central_status_changed_cb_t callback) {
    ARG_UNUSED(callback);
    return 0;
}

static const struct zmk_split_transport_central_api central_api = {
    .send_command = central_send_command,
    .get_available_source_ids = central_get_available_source_ids,
    .set_enabled = central_set_enabled,
    .get_status = central_get_status,
    .set_status_callback = central_set_status_callback,
};

ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(esb_central, &central_api, CONFIG_ZMK_SPLIT_ESB_PRIORITY);

enum central_inbound_kind {
    CENTRAL_INBOUND_EVENT = 0,
    CENTRAL_INBOUND_KEEPALIVE = 1,
};

struct central_inbound {
    uint8_t source;
    uint8_t kind;
    union {
        struct zmk_split_transport_peripheral_event event;
        struct {
            uint8_t position_bitmap[ESB_KEEPALIVE_BITMAP_BYTES];
            uint8_t battery_level;
        } keepalive;
    } data;
};

/* ZMK behavior state has no locks, its timers fire on system workqueue:
 * behavior-bound events must run there too.
 * Input events bypass, input_report is safe from any context. */
K_MSGQ_DEFINE(central_event_msgq, sizeof(struct central_inbound),
              CONFIG_ZMK_SPLIT_ESB_EVENT_QUEUE_SIZE, 4);

#define CENTRAL_PIPE_MAX 8 /* ESB hardware pipe count */
static uint8_t tracked_positions[CENTRAL_PIPE_MAX][ESB_KEEPALIVE_BITMAP_BYTES];
static uint8_t tracked_battery_levels[CENTRAL_PIPE_MAX];

static void forward_key_position(uint8_t source, uint32_t position, bool pressed) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data = {.key_position_event = {
                     .position = position,
                     .pressed = pressed,
                 }},
    };
    zmk_split_transport_central_peripheral_event_handler(&esb_central, source, event);
}

static void reconcile_positions(uint8_t source, const uint8_t *received) {
    if (source >= CENTRAL_PIPE_MAX) {
        return;
    }
    for (uint32_t position = 0; position < ESB_KEEPALIVE_POSITION_COUNT; position++) {
        bool tracked = esb_keepalive_bitmap_get(tracked_positions[source], position);
        bool pressed = esb_keepalive_bitmap_get(received, position);
        if (tracked == pressed) {
            continue;
        }
        LOG_WRN("Reconciling lost %s of position %u from %u", pressed ? "press" : "release",
                (unsigned int)position, source);
        esb_keepalive_bitmap_set(tracked_positions[source], position, pressed);
        forward_key_position(source, position, pressed);
    }
}

/* Heal stream inconsistencies from radio loss before ZMK sees them.
 * Orphan release (lost press) drops.
 * Repeated press (lost release) synthesizes the missing release first.
 * Positions beyond the bitmap pass through. */
static void deliver_key_event(uint8_t source,
                              const struct zmk_split_transport_peripheral_event *event) {
    uint32_t position = event->data.key_position_event.position;
    bool pressed = event->data.key_position_event.pressed;
    if (source >= CENTRAL_PIPE_MAX || position >= ESB_KEEPALIVE_POSITION_COUNT) {
        zmk_split_transport_central_peripheral_event_handler(&esb_central, source, *event);
        return;
    }
    bool tracked = esb_keepalive_bitmap_get(tracked_positions[source], position);
    if (!pressed && !tracked) {
        LOG_WRN("Dropping orphan release of position %u from %u", (unsigned int)position, source);
        return;
    }
    if (pressed && tracked) {
        LOG_WRN("Healing lost release of position %u from %u", (unsigned int)position, source);
        forward_key_position(source, position, false);
    }
    esb_keepalive_bitmap_set(tracked_positions[source], position, pressed);
    zmk_split_transport_central_peripheral_event_handler(&esb_central, source, *event);
}

/* ZMK battery handling sits behind BLE split Kconfig, unreachable with
 * ZMK_SPLIT_BLE=n: raise peripheral battery events here.
 * Level arrives twice, as a change event and in every snapshot: tracked levels
 * dedup, and a lost change event heals at the next keepalive.
 * Caller gates on ZMK_BATTERY_REPORTING, sole provider of the event's
 * ZMK_EVENT_IMPL. */
static void reconcile_battery(uint8_t source, uint8_t level) {
    if (source >= CENTRAL_PIPE_MAX || level == ESB_KEEPALIVE_BATTERY_UNKNOWN) {
        return;
    }
    if (tracked_battery_levels[source] == level) {
        return;
    }
    tracked_battery_levels[source] = level;
    LOG_DBG("peripheral %u battery %u%%", source, level);
    struct zmk_peripheral_battery_state_changed battery_event = {
        .source = source,
        .state_of_charge = level,
    };
    raise_zmk_peripheral_battery_state_changed(battery_event);
}

static void central_event_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct central_inbound inbound;
    while (k_msgq_get(&central_event_msgq, &inbound, K_NO_WAIT) == 0) {
        if (inbound.kind == CENTRAL_INBOUND_KEEPALIVE) {
            reconcile_positions(inbound.source, inbound.data.keepalive.position_bitmap);
            if (IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) &&
                !IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)) {
                reconcile_battery(inbound.source, inbound.data.keepalive.battery_level);
            }
            continue;
        }
        if (inbound.data.event.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
            deliver_key_event(inbound.source, &inbound.data.event);
            continue;
        }
        if (inbound.data.event.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT &&
            !IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)) {
            if (IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)) {
                reconcile_battery(inbound.source, inbound.data.event.data.battery_event.level);
            }
            /* Never falls through: upstream handler warns unknown-type per report. */
            continue;
        }
        zmk_split_transport_central_peripheral_event_handler(&esb_central, inbound.source,
                                                             inbound.data.event);
    }
}

static K_WORK_DEFINE(central_event_work, central_event_work_fn);

static void central_on_rx(uint8_t pipe, const uint8_t *data, size_t length) {
    if (esb_keepalive_matches(data, (uint8_t)length)) {
        struct central_inbound inbound = {
            .source = pipe,
            .kind = CENTRAL_INBOUND_KEEPALIVE,
        };
        memcpy(inbound.data.keepalive.position_bitmap, esb_keepalive_bitmap(data),
               ESB_KEEPALIVE_BITMAP_BYTES);
        inbound.data.keepalive.battery_level = esb_keepalive_battery_level(data);
        if (k_msgq_put(&central_event_msgq, &inbound, K_NO_WAIT) < 0) {
            return; /* next keepalive retries */
        }
        k_work_submit(&central_event_work);
        return;
    }
    /* One packet may carry several coalesced events.
     * Decode and replay each in order. */
    size_t offset = 0;
    while (offset < length) {
        struct zmk_split_transport_peripheral_event event;
        size_t consumed = esb_wire_decode_event(&data[offset], length - offset, &event);
        if (consumed == 0) {
            LOG_WRN("Dropping packet, undecodable event at offset %u", (unsigned int)offset);
            return;
        }
        offset += consumed;
        if (event.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT) {
            zmk_split_transport_central_peripheral_event_handler(&esb_central, pipe, event);
            continue;
        }
        struct central_inbound inbound = {
            .source = pipe,
            .kind = CENTRAL_INBOUND_EVENT,
            .data = {.event = event},
        };
        if (k_msgq_put(&central_event_msgq, &inbound, K_NO_WAIT) < 0) {
            LOG_WRN("Dropping event, central event queue full");
            continue;
        }
        k_work_submit(&central_event_work);
    }
}

static int central_init(void) {
    /* Zero-init would swallow a genuine first 0% report. */
    for (uint8_t pipe = 0; pipe < CENTRAL_PIPE_MAX; pipe++) {
        tracked_battery_levels[pipe] = ESB_KEEPALIVE_BATTERY_UNKNOWN;
    }
    return esb_link_init(central_on_rx);
}

SYS_INIT(central_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
