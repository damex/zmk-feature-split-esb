// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "central.h"
#include "hop.h"
#include "wire_central.h"
#include "wire_link.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_esb_wire_peer),
             "central wire peer needs a chosen zmk,esb-wire-peer");
BUILD_ASSERT(!DT_ENUM_HAS_VALUE(DT_CHOSEN(zmk_esb_wire_peer), role, self),
             "wire peer pipe cannot have role self");

static const uint8_t peer_pipe = DT_PROP(DT_CHOSEN(zmk_esb_wire_peer), pipe);

bool wire_central_owns_pipe(uint8_t pipe) {
    return pipe == peer_pipe;
}

int wire_central_send_command(const uint8_t *data, size_t length) {
    return wire_link_send_event(data, length);
}

bool wire_central_peer_is_up(void) {
    return wire_link_is_up();
}

static void wire_central_on_frame(const uint8_t *payload, size_t length, void *user_data) {
    ARG_UNUSED(user_data);
    hop_pipe_note_seen(peer_pipe);
    central_ingest_packet(peer_pipe, payload, length);
}

static struct wire_link_subscription wire_central_subscription = {
    .callback = wire_central_on_frame,
    .user_data = NULL,
};

static int wire_central_init(void) {
    return wire_link_register_rx(&wire_central_subscription);
}

SYS_INIT(wire_central_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
