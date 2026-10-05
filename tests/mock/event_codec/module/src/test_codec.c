// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Checks for the on-air split peripheral event codec.
 * Exits 0 once every check passes, 1 at the first failing one.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/sensor.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/types.h>

#include "esb_keepalive.h"
#include "esb_wire.h"
#include "mock.h"

#define UNKNOWN_EVENT_TYPE 0x7F
#define EVENT_TYPE_OFFSET 0

static const struct zmk_split_transport_peripheral_event key_event = {
    .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
    .data.key_position_event = {.position = 5, .pressed = 1},
};

static const struct zmk_split_transport_peripheral_event sensor_event = {
    .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT,
    .data.sensor_event = {
        .channel_data = {.value = {.val1 = 15, .val2 = 500000}, .channel = SENSOR_CHAN_ROTATION},
        .sensor_index = 1,
    },
};

static const struct zmk_split_transport_peripheral_event input_event = {
    .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
    .data.input_event = {.reg = 1, .sync = 1, .type = INPUT_EV_REL, .code = INPUT_REL_X,
                         .value = -37},
};

static const struct zmk_split_transport_peripheral_event battery_event = {
    .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
    .data.battery_event = {.level = 87},
};

static bool events_equal(const struct zmk_split_transport_peripheral_event *left,
                         const struct zmk_split_transport_peripheral_event *right) {
    return memcmp(left, right, sizeof(*left)) == 0;
}

static void check_round_trip(const struct zmk_split_transport_peripheral_event *event,
                             size_t expected_length, const char *what) {
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    size_t encoded = esb_wire_encode_event(wire, sizeof(wire), event);
    mock_check(encoded == expected_length, what);
    struct zmk_split_transport_peripheral_event decoded;
    size_t consumed = esb_wire_decode_event(wire, encoded, &decoded);
    mock_check(consumed == encoded, what);
    mock_check(events_equal(&decoded, event), what);
}

static void check_round_trips(void) {
    check_round_trip(&key_event, 1 + sizeof(key_event.data.key_position_event),
                     "key position event round trip");
    check_round_trip(&sensor_event, 1 + sizeof(sensor_event.data.sensor_event),
                     "sensor event round trip");
    check_round_trip(&input_event, ESB_WIRE_INPUT_EVENT_SIZE,
                     "input event round trip at packed size");
    check_round_trip(&battery_event, 1 + sizeof(battery_event.data.battery_event),
                     "battery event round trip");
}

static void check_rejections(void) {
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE] = {0};
    struct zmk_split_transport_peripheral_event decoded;
    struct zmk_split_transport_peripheral_event unknown = key_event;
    unknown.type = (enum zmk_split_transport_peripheral_event_type)UNKNOWN_EVENT_TYPE;
    mock_check(esb_wire_encode_event(wire, sizeof(wire), &unknown) == 0,
               "unknown type does not encode");
    wire[EVENT_TYPE_OFFSET] = UNKNOWN_EVENT_TYPE;
    mock_check(esb_wire_decode_event(wire, sizeof(wire), &decoded) == 0,
               "unknown type does not decode");
    wire[EVENT_TYPE_OFFSET] = ESB_KEEPALIVE_TAG;
    mock_check(esb_wire_decode_event(wire, sizeof(wire), &decoded) == 0,
               "keepalive tag does not decode as an event");
    mock_check(esb_wire_encode_event(wire, ESB_WIRE_INPUT_EVENT_SIZE - 1, &input_event) == 0,
               "input event does not encode into a short buffer");
    size_t encoded = esb_wire_encode_event(wire, sizeof(wire), &input_event);
    mock_check(esb_wire_decode_event(wire, encoded - 1, &decoded) == 0,
               "truncated input event does not decode");
    mock_check(esb_wire_decode_event(wire, 0, &decoded) == 0, "empty buffer does not decode");
}

static void check_coalesced_packet(void) {
    const struct zmk_split_transport_peripheral_event *events[] = {
        &key_event,
        &input_event,
        &battery_event,
    };
    uint8_t packet[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
    size_t length = 0;
    for (size_t index = 0; index < ARRAY_SIZE(events); index++) {
        size_t encoded = esb_wire_encode_event(&packet[length], sizeof(packet) - length,
                                               events[index]);
        mock_check(encoded > 0, "coalesced event encodes");
        length += encoded;
    }
    size_t offset = 0;
    for (size_t index = 0; index < ARRAY_SIZE(events); index++) {
        struct zmk_split_transport_peripheral_event decoded;
        size_t consumed = esb_wire_decode_event(&packet[offset], length - offset, &decoded);
        mock_check(consumed > 0, "coalesced event decodes");
        mock_check(events_equal(&decoded, events[index]), "coalesced events decode in order");
        offset += consumed;
    }
    mock_check(offset == length, "coalesced packet decodes to its end");
}

static int test_codec_init(void) {
    check_round_trips();
    check_rejections();
    check_coalesced_packet();
    printk("PASS: all %u event codec checks\n", (unsigned int)mock_checks_passed());
    exit(0);
    return 0;
}
SYS_INIT(test_codec_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
