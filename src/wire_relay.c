// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Wire relay between the wire peer and its ESB pipe. */

#include "wire_relay.h"

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "esb_link.h"
#include "wire_link.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static const uint8_t peer_pipe = DT_PROP(DT_CHOSEN(zmk_esb_wire_peer), pipe);

bool wire_relay_owns_pipe(uint8_t pipe) {
    return pipe == peer_pipe;
}

void wire_relay_forward_to_peer(const uint8_t *data, size_t length) {
    if (!wire_link_can_transmit()) {
        return;
    }
    int error = wire_link_send_event(data, length);
    if (error != 0) {
        LOG_WRN("wire relay to peer failed (%d)", error);
    }
}

static void wire_relay_on_frame(const uint8_t *payload, size_t length, void *user_data) {
    ARG_UNUSED(user_data);
    int error = esb_link_send_relay(payload, length, true);
    if (error != 0) {
        LOG_WRN("wire relay send failed (%d)", error);
    }
}

static struct wire_link_subscription wire_relay_subscription = {
    .callback = wire_relay_on_frame,
    .user_data = NULL,
};

static int wire_relay_init(void) {
    return wire_link_register_rx(&wire_relay_subscription);
}

SYS_INIT(wire_relay_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
