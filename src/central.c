// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * ZMK ESB split central. Source id = ESB pipe.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

#include <zmk/events/split_esb_peripheral_changed.h>
#include <zmk/pointing/input_split.h>
#include <zmk/split/transport/central.h>
#include <zmk/split/transport/types.h>

#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "esb_wire.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct zmk_split_transport_peripheral_event) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "peripheral event does not fit in one ESB payload; raise ZMK_SPLIT_ESB_MAX_PAYLOAD");
BUILD_ASSERT(sizeof(struct zmk_split_transport_central_command) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "central command does not fit in one ESB payload; raise ZMK_SPLIT_ESB_MAX_PAYLOAD");

static bool transport_enabled;
static zmk_split_transport_central_status_changed_cb_t status_cb;

static enum zmk_split_transport_connections_status central_connections_status(void);

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
        .connections = central_connections_status(),
    };
}

static int
central_set_status_callback(zmk_split_transport_central_status_changed_cb_t callback) {
    status_cb = callback;
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
        uint8_t position_bitmap[ESB_KEEPALIVE_BITMAP_BYTES];
    } data;
};

/* ZMK behavior state has no locks, its timers fire on system workqueue:
 * behavior-bound events must run there too.
 * Input events bypass, input_report is safe from any context. */
K_MSGQ_DEFINE(central_event_msgq, sizeof(struct central_inbound),
              CONFIG_ZMK_SPLIT_ESB_EVENT_QUEUE_SIZE, 4);

#define CENTRAL_PIPE_MAX 8 /* ESB hardware pipe count */
static uint8_t tracked_positions[CENTRAL_PIPE_MAX][ESB_KEEPALIVE_BITMAP_BYTES];

#define CENTRAL_INPUT_REG_MAX 32
static const uint32_t peripheral_timeout_ms = DT_INST_PROP(0, peripheral_timeout_ms);

/* Written on the RX thread, read by the staleness tick: single writer per slot,
 * aligned loads, no lock needed. */
static uint32_t pipe_last_heard_ms[CENTRAL_PIPE_MAX];
static bool pipe_heard[CENTRAL_PIPE_MAX];
static uint32_t pipe_seen_input_regs[CENTRAL_PIPE_MAX];

static bool pipe_stale[CENTRAL_PIPE_MAX];
static bool pipe_connected[CENTRAL_PIPE_MAX];
static enum zmk_split_transport_connections_status last_connections =
    ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;

static enum zmk_split_transport_connections_status central_connections_status(void) {
    uint8_t total = 0;
    uint8_t connected = 0;
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count && pipe < CENTRAL_PIPE_MAX; pipe++) {
        total++;
        if (pipe_connected[pipe]) {
            connected++;
        }
    }
    if (connected == 0) {
        return ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;
    }
    if (connected < total) {
        return ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED;
    }
    return ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;
}

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
    uint8_t *tracked = tracked_positions[source];
    for (uint32_t position = esb_keepalive_bitmap_diff_next(tracked, received, 0);
         position < ESB_KEEPALIVE_POSITION_COUNT;
         position = esb_keepalive_bitmap_diff_next(tracked, received, position + 1)) {
        bool pressed = esb_keepalive_bitmap_get(received, position);
        LOG_WRN("Reconciling lost %s of position %u from %u", pressed ? "press" : "release",
                (unsigned int)position, source);
        esb_keepalive_bitmap_set(tracked, position, pressed);
        forward_key_position(source, position, pressed);
    }
}

static void deliver_key_event(uint8_t source,
                              const struct zmk_split_transport_peripheral_event *event) {
    uint32_t position = event->data.key_position_event.position;
    bool pressed = event->data.key_position_event.pressed;
    if (source >= CENTRAL_PIPE_MAX || position >= ESB_KEEPALIVE_POSITION_COUNT) {
        zmk_split_transport_central_peripheral_event_handler(&esb_central, source, *event);
        return;
    }
    switch (esb_keepalive_key_verdict(tracked_positions[source], position, pressed)) {
    case ESB_KEEPALIVE_KEY_DROP_ORPHAN_RELEASE:
        LOG_WRN("Dropping orphan release of position %u from %u", (unsigned int)position, source);
        return;
    case ESB_KEEPALIVE_KEY_HEAL_LOST_RELEASE:
        LOG_WRN("Healing lost release of position %u from %u", (unsigned int)position, source);
        forward_key_position(source, position, false);
        break;
    case ESB_KEEPALIVE_KEY_FORWARD:
        break;
    default:
        __ASSERT_NO_MSG(false);
        return;
    }
    esb_keepalive_bitmap_set(tracked_positions[source], position, pressed);
    zmk_split_transport_central_peripheral_event_handler(&esb_central, source, *event);
}

static void release_stale_pipe(uint8_t pipe) {
    LOG_WRN("Releasing held state of silent peripheral %u", pipe);
    static const uint8_t no_positions[ESB_KEEPALIVE_BITMAP_BYTES];
    uint8_t *tracked = tracked_positions[pipe];
    for (uint32_t position = esb_keepalive_bitmap_diff_next(tracked, no_positions, 0);
         position < ESB_KEEPALIVE_POSITION_COUNT;
         position = esb_keepalive_bitmap_diff_next(tracked, no_positions, position + 1)) {
        esb_keepalive_bitmap_set(tracked, position, false);
        forward_key_position(pipe, position, false);
    }
    if (IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)) {
        for (uint8_t reg = 0; reg < CENTRAL_INPUT_REG_MAX; reg++) {
            if ((pipe_seen_input_regs[pipe] & BIT(reg)) != 0) {
                zmk_input_split_peripheral_disconnected(reg);
            }
        }
    }
}

#define STALENESS_CHECK_PERIOD_MS 500

static void staleness_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(staleness_work, staleness_work_fn);

static void staleness_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t now = k_uptime_get_32();
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count && pipe < CENTRAL_PIPE_MAX; pipe++) {
        if (pipe_heard[pipe]) {
            if ((now - pipe_last_heard_ms[pipe]) <= peripheral_timeout_ms) {
                pipe_stale[pipe] = false;
            } else if (!pipe_stale[pipe]) {
                pipe_stale[pipe] = true;
                release_stale_pipe(pipe);
            }
        }
        bool connected = pipe_heard[pipe] && !pipe_stale[pipe];
        if (connected != pipe_connected[pipe]) {
            pipe_connected[pipe] = connected;
            raise_zmk_split_esb_peripheral_changed(
                (struct zmk_split_esb_peripheral_changed){.source = pipe, .connected = connected});
        }
    }
    enum zmk_split_transport_connections_status connections = central_connections_status();
    if (connections != last_connections) {
        last_connections = connections;
        if (status_cb != NULL) {
            status_cb(&esb_central, central_get_status());
        }
    }
    k_work_reschedule(&staleness_work, K_MSEC(STALENESS_CHECK_PERIOD_MS));
}

static void central_event_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct central_inbound inbound;
    while (k_msgq_get(&central_event_msgq, &inbound, K_NO_WAIT) == 0) {
        if (inbound.kind == CENTRAL_INBOUND_KEEPALIVE) {
            reconcile_positions(inbound.source, inbound.data.position_bitmap);
            continue;
        }
        if (inbound.data.event.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
            deliver_key_event(inbound.source, &inbound.data.event);
            continue;
        }
        zmk_split_transport_central_peripheral_event_handler(&esb_central, inbound.source,
                                                             inbound.data.event);
    }
}

static K_WORK_DEFINE(central_event_work, central_event_work_fn);

static void central_on_rx(uint8_t pipe, const uint8_t *data, size_t length) {
    if (pipe < CENTRAL_PIPE_MAX) {
        pipe_last_heard_ms[pipe] = k_uptime_get_32();
        pipe_heard[pipe] = true;
    }
    if (esb_keepalive_matches(data, (uint8_t)length)) {
        struct central_inbound inbound = {
            .source = pipe,
            .kind = CENTRAL_INBOUND_KEEPALIVE,
        };
        memcpy(inbound.data.position_bitmap, esb_keepalive_bitmap(data),
               ESB_KEEPALIVE_BITMAP_BYTES);
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
            LOG_HEXDUMP_DBG(data, length, "undecodable payload");
            return;
        }
        offset += consumed;
        if (event.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT) {
            if (pipe < CENTRAL_PIPE_MAX && event.data.input_event.reg < CENTRAL_INPUT_REG_MAX) {
                pipe_seen_input_regs[pipe] |= BIT(event.data.input_event.reg);
            }
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
    k_work_reschedule(&staleness_work, K_MSEC(STALENESS_CHECK_PERIOD_MS));
    return esb_link_init(central_on_rx);
}

SYS_INIT(central_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
