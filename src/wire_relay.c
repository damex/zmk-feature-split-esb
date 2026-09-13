// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "esb_link.h"
#include "wire_link.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

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
