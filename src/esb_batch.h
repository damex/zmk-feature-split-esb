// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Input event batching into one ESB packet. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <zmk/split/transport/types.h>

#include "esb_wire.h"

#define ESB_BATCH_MAX (CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD / ESB_WIRE_INPUT_EVENT_SIZE)

struct esb_batch {
    struct zmk_split_transport_peripheral_event events[ESB_BATCH_MAX];
    size_t count;
    bool wants_ack;
};

/* Input events only, single producer (the input thread), so no lock.
 * Buffer the event, flush on its sync flag or a full batch.
 * Caller routes non-input events straight to esb_link_send. */
int esb_batch_report_event(struct esb_batch *batch,
                           const struct zmk_split_transport_peripheral_event *event,
                           bool wants_ack);

int esb_batch_flush(struct esb_batch *batch);
