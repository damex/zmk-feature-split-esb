// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Peripheral half of the ESB radio layer: uplink send and keepalive.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"
#include "hop_policy.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_esb_self), "peripheral needs a chosen zmk,esb-self");
static const uint8_t self_pipe = DT_PROP(DT_CHOSEN(zmk_esb_self), pipe);

int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    if (length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    hop_note_data_sent();
    struct esb_payload payload = {0};
    payload.pipe = self_pipe;
    payload.noack = !ack;
    payload.length = (uint8_t)length;
    memcpy(payload.data, data, length);
    int error = esb_write_payload(&payload);
    if (error) {
        LOG_WRN("uplink event dropped, esb_write_payload returned %d", error);
    }
    return error;
}

void esb_link_send_keepalive(uint8_t state) {
    struct esb_payload keepalive = {0};
    keepalive.pipe = self_pipe;
    keepalive.length = ESB_KEEPALIVE_LENGTH;
    keepalive.data[0] = state;
    (void)esb_write_payload(&keepalive);
}

int esb_link_role_start(void) {
    return 0;
}

void esb_link_role_rx_done(void) {
}
