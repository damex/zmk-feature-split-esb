// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Wire transport for a split peripheral. */

#include "peripheral.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/activity.h>
#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include "esb_wire.h"
#include "wire_link.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static bool transport_enabled;
static uint32_t last_wire_send_ms;
static uint8_t wire_keepalive_buffer[PERIPHERAL_KEEPALIVE_MAX_LENGTH];

static int wire_peripheral_send(const uint8_t *data, size_t length) {
    int result = wire_link_send(data, length);
    if (result == 0) {
        last_wire_send_ms = k_uptime_get_32();
    }
    return result;
}

static int wire_peripheral_report_event(const struct zmk_split_transport_peripheral_event *event) {
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    int encoded = peripheral_encode_event(event, wire, sizeof(wire));
    if (encoded < 0) {
        return encoded;
    }
    return wire_peripheral_send(wire, (size_t)encoded);
}

static int wire_peripheral_set_enabled(bool enabled) {
    transport_enabled = enabled;
    return 0;
}

static struct zmk_split_transport_status wire_peripheral_get_status(void) {
    return (struct zmk_split_transport_status){
        .available = true,
        .enabled = transport_enabled,
        .connections = wire_link_is_up()
                           ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                           : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}

static int
wire_peripheral_set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t callback) {
    ARG_UNUSED(callback);
    return 0;
}

static const struct zmk_split_transport_peripheral_api wire_peripheral_api = {
    .report_event = wire_peripheral_report_event,
    .set_enabled = wire_peripheral_set_enabled,
    .get_status = wire_peripheral_get_status,
    .set_status_callback = wire_peripheral_set_status_callback,
};

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(wire_peripheral, &wire_peripheral_api,
                                        CONFIG_ZMK_SPLIT_ESB_PRIORITY);

static void wire_peripheral_on_frame(const uint8_t *payload, size_t length, void *user_data) {
    ARG_UNUSED(user_data);
    peripheral_deliver_command(payload, length);
}

static struct wire_link_subscription wire_peripheral_subscription = {
    .callback = wire_peripheral_on_frame,
    .user_data = NULL,
};

static void wire_peripheral_keepalive_fire(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(wire_peripheral_keepalive_work, wire_peripheral_keepalive_fire);

static void wire_peripheral_keepalive_fire(struct k_work *work) {
    ARG_UNUSED(work);
    const uint32_t now_ms = k_uptime_get_32();
    const uint32_t last_ms = last_wire_send_ms;
    const uint32_t idle_ms = now_ms - last_ms;
    if (idle_ms < CONFIG_ZMK_SPLIT_ESB_WIRE_STATE_MS) {
        k_work_reschedule(&wire_peripheral_keepalive_work,
                          K_MSEC(CONFIG_ZMK_SPLIT_ESB_WIRE_STATE_MS - idle_ms));
        return;
    }
    const bool active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
    uint8_t length = peripheral_keepalive_fill(wire_keepalive_buffer,
                                               sizeof(wire_keepalive_buffer),
                                               active ? ESB_KEEPALIVE_ACTIVE : ESB_KEEPALIVE_IDLE,
                                               0);
    if (length > 0) {
        (void)wire_peripheral_send(wire_keepalive_buffer, length);
    }
    k_work_reschedule(&wire_peripheral_keepalive_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_WIRE_STATE_MS));
}

static int wire_peripheral_init(void) {
    peripheral_set_transport(&wire_peripheral);
    int error = wire_link_register_rx(&wire_peripheral_subscription);
    if (error != 0) {
        return error;
    }
    k_work_reschedule(&wire_peripheral_keepalive_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_WIRE_STATE_MS));
    return 0;
}

SYS_INIT(wire_peripheral_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
