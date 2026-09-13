// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Wire transport for a split peripheral. */

#include "peripheral.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include "esb_wire.h"
#include "wire_link.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static bool transport_enabled;

static int wire_peripheral_report_event(const struct zmk_split_transport_peripheral_event *event) {
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    int encoded = peripheral_encode_event(event, wire, sizeof(wire));
    if (encoded < 0) {
        return encoded;
    }
    return wire_link_send(wire, (size_t)encoded);
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

static int wire_peripheral_init(void) {
    peripheral_set_transport(&wire_peripheral);
    return wire_link_register_rx(&wire_peripheral_subscription);
}

SYS_INIT(wire_peripheral_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
