// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * ZMK ESB split peripheral.
 */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include "esb_link.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct zmk_split_transport_peripheral_event) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "peripheral event does not fit in one ESB payload; raise ZMK_SPLIT_ESB_MAX_PAYLOAD");
BUILD_ASSERT(sizeof(struct zmk_split_transport_central_command) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "central command does not fit in one ESB payload; raise ZMK_SPLIT_ESB_MAX_PAYLOAD");

static zmk_split_transport_peripheral_status_changed_cb_t status_callback;
static bool transport_enabled;

static bool event_wants_ack(const struct zmk_split_transport_peripheral_event *event) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_LOSSY_INPUT)
    /* Motion is high-rate and self-correcting: drop the ACK for lower latency. */
    return event->type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT;
#else
    ARG_UNUSED(event);
    return true;
#endif
}

static int peripheral_report_event(const struct zmk_split_transport_peripheral_event *event) {
    return esb_link_send((const uint8_t *)event, sizeof(*event), event_wants_ack(event));
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
    status_callback = callback;
    return 0;
}

static const struct zmk_split_transport_peripheral_api peripheral_api = {
    .report_event = peripheral_report_event,
    .set_enabled = peripheral_set_enabled,
    .get_status = peripheral_get_status,
    .set_status_callback = peripheral_set_status_callback,
};

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(esb_peripheral, &peripheral_api, CONFIG_ZMK_SPLIT_ESB_PRIORITY);

/* Runs in the esb_link work thread (not the radio ISR). */
static void peripheral_on_rx(const uint8_t *data, size_t len) {
    if (len != sizeof(struct zmk_split_transport_central_command)) {
        LOG_WRN("Dropping command with unexpected size %u", (unsigned int)len);
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
