// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <string.h>

#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include "esb_wire.h"

/* ZMK pads input_event to 12 bytes, the union's widest member.
 * Packing its fields drops the on-air payload to 9. */
struct __packed input_wire {
    uint8_t reg;
    uint8_t sync;
    uint8_t type;
    uint16_t code;
    int32_t value;
};
BUILD_ASSERT(sizeof(struct input_wire) == 9, "input wire layout drifted");
BUILD_ASSERT(1 + sizeof(struct input_wire) == ESB_WIRE_INPUT_EVENT_SIZE,
             "ESB_WIRE_INPUT_EVENT_SIZE drifted from the encoded layout");

#define WIRE_PAYLOAD(member)                                                                       \
    (uint8_t)sizeof(((struct zmk_split_transport_peripheral_event *)0)->data.member)

/* On-air payload size per event type. Input is the packed size, others copy their
 * union member verbatim. A zero slot marks an unknown tag for the decoder. */
static const uint8_t payload_size[] = {
    [ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT] = WIRE_PAYLOAD(key_position_event),
    [ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT] = WIRE_PAYLOAD(sensor_event),
    [ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT] = sizeof(struct input_wire),
    [ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT] = WIRE_PAYLOAD(battery_event),
};

static bool is_input(uint8_t type) {
    return type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT;
}

size_t esb_wire_encode_event(uint8_t *out, size_t out_cap,
                             const struct zmk_split_transport_peripheral_event *event) {
    __ASSERT_NO_MSG(out != NULL);
    __ASSERT_NO_MSG(event != NULL);
    uint8_t type = (uint8_t)event->type;
    uint8_t size = (type < ARRAY_SIZE(payload_size)) ? payload_size[type] : 0;
    if (out_cap < (size_t)(1 + size)) {
        return 0;
    }
    out[0] = type;
    if (is_input(type)) {
        struct input_wire wire = {
            .reg = event->data.input_event.reg,
            .sync = event->data.input_event.sync,
            .type = event->data.input_event.type,
            .code = event->data.input_event.code,
            .value = event->data.input_event.value,
        };
        memcpy(&out[1], &wire, sizeof(wire));
    } else {
        memcpy(&out[1], &event->data, size);
    }
    return (size_t)(1 + size);
}

size_t esb_wire_decode_event(const uint8_t *in, size_t avail,
                             struct zmk_split_transport_peripheral_event *event) {
    __ASSERT_NO_MSG(in != NULL);
    __ASSERT_NO_MSG(event != NULL);
    if (avail < 1) {
        return 0;
    }
    uint8_t type = in[0];
    if (type >= ARRAY_SIZE(payload_size) || payload_size[type] == 0) {
        return 0;
    }
    uint8_t size = payload_size[type];
    if (avail < (size_t)(1 + size)) {
        return 0;
    }
    *event = (struct zmk_split_transport_peripheral_event){0};
    event->type = (enum zmk_split_transport_peripheral_event_type)type;
    if (is_input(type)) {
        struct input_wire wire;
        memcpy(&wire, &in[1], sizeof(wire));
        event->data.input_event.reg = wire.reg;
        event->data.input_event.sync = wire.sync;
        event->data.input_event.type = wire.type;
        event->data.input_event.code = wire.code;
        event->data.input_event.value = wire.value;
    } else {
        memcpy(&event->data, &in[1], size);
    }
    return (size_t)(1 + size);
}
