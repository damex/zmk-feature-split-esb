// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <string.h>

#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include "esb_wire.h"

#define WIRE_PAYLOAD(member)                                                                       \
    (uint8_t)sizeof(((struct zmk_split_transport_peripheral_event *)0)->data.member)

/* Union payload size per event type, direct-indexed (the type enum is dense from 0).
 * A zero slot marks an unknown tag for the decoder. */
static const uint8_t payload_size[] = {
    [ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT] = WIRE_PAYLOAD(key_position_event),
    [ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT] = WIRE_PAYLOAD(sensor_event),
    [ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT] = WIRE_PAYLOAD(input_event),
    [ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT] = WIRE_PAYLOAD(battery_event),
};

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
    memcpy(&out[1], &event->data, size);
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
    memcpy(&event->data, &in[1], size);
    return (size_t)(1 + size);
}
