// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * ZMK split peripheral shared core.
 * Common state and dispatch across transports.
 */

#include "peripheral.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/battery.h>
#include <zmk/sensors.h>

#include <zmk_split_esb.h>

#include "esb_hid_state.h"
#include "esb_keepalive.h"
#include "esb_sensor_sync.h"
#include "esb_wire.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct zmk_split_transport_central_command) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "central command does not fit in one ESB payload; raise ZMK_SPLIT_ESB_MAX_PAYLOAD");

static uint8_t pressed_positions[ESB_KEEPALIVE_BITMAP_BYTES];

const uint8_t *peripheral_pressed_bitmap(void) {
    return pressed_positions;
}

void peripheral_note_key_event(uint8_t position, bool pressed) {
    esb_keepalive_bitmap_set(pressed_positions, position, pressed);
}

uint8_t peripheral_battery_level(void) {
    if (!IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)) {
        return ESB_KEEPALIVE_BATTERY_UNKNOWN;
    }
    uint8_t level = zmk_battery_state_of_charge();
    /* Zero is pre-first-sample, not 0% charge. */
    return (level == 0) ? ESB_KEEPALIVE_BATTERY_UNKNOWN : level;
}

#if ZMK_KEYMAP_HAS_SENSORS
BUILD_ASSERT(ESB_KEEPALIVE_LENGTH(ZMK_KEYMAP_SENSORS_LEN) <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "keepalive with sensor totals does not fit one ESB payload");

static int64_t sensor_total_udeg[ZMK_KEYMAP_SENSORS_LEN];

uint8_t peripheral_sensor_count(void) {
    return ZMK_KEYMAP_SENSORS_LEN;
}

const int64_t *peripheral_sensor_totals(void) {
    return sensor_total_udeg;
}

void peripheral_sensor_event_to_total(struct zmk_split_transport_peripheral_event *event) {
    uint8_t sensor_index = event->data.sensor_event.sensor_index;
    if (sensor_index >= ARRAY_SIZE(sensor_total_udeg)) {
        return;
    }
    struct sensor_value value = event->data.sensor_event.channel_data.value;
    sensor_total_udeg[sensor_index] += esb_sensor_udeg(value.val1, value.val2);
    value.val1 = esb_sensor_udeg_val1(sensor_total_udeg[sensor_index]);
    value.val2 = esb_sensor_udeg_val2(sensor_total_udeg[sensor_index]);
    event->data.sensor_event.channel_data.value = value;
}
#else
uint8_t peripheral_sensor_count(void) {
    return 0;
}

const int64_t *peripheral_sensor_totals(void) {
    return NULL;
}

void peripheral_sensor_event_to_total(struct zmk_split_transport_peripheral_event *event) {
    ARG_UNUSED(event);
}
#endif

int peripheral_encode_event(const struct zmk_split_transport_peripheral_event *event,
                            uint8_t *wire, size_t wire_size) {
    if (event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        peripheral_note_key_event(event->data.key_position_event.position,
                                  event->data.key_position_event.pressed);
    }
    struct zmk_split_transport_peripheral_event mutated;
    if (event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT) {
        mutated = *event;
        peripheral_sensor_event_to_total(&mutated);
        event = &mutated;
    }
    size_t length = esb_wire_encode_event(wire, wire_size, event);
    if (length == 0) {
        return -ENOTSUP;
    }
    return (int)length;
}

/* Commands invoke behaviors, same single-context contract as central events. */
K_MSGQ_DEFINE(peripheral_command_msgq, sizeof(struct zmk_split_transport_central_command),
              CONFIG_ZMK_SPLIT_ESB_COMMAND_QUEUE_SIZE, 4);

static const struct zmk_split_transport_peripheral *transport_instance;

void peripheral_set_transport(const struct zmk_split_transport_peripheral *instance) {
    transport_instance = instance;
}

static void peripheral_command_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct zmk_split_transport_central_command command;
    while (k_msgq_get(&peripheral_command_msgq, &command, K_NO_WAIT) == 0) {
        if (transport_instance != NULL) {
            zmk_split_transport_peripheral_command_handler(transport_instance, command);
        }
    }
}

static K_WORK_DEFINE(peripheral_command_work, peripheral_command_work_fn);

void peripheral_deliver_command(const uint8_t *bytes, size_t length) {
    if (length != sizeof(struct zmk_split_transport_central_command)) {
        LOG_WRN("Dropping command with unexpected size %u", (unsigned int)length);
        LOG_HEXDUMP_DBG(bytes, length, "unexpected command payload");
        return;
    }
    struct zmk_split_transport_central_command command;
    memcpy(&command, bytes, sizeof(command));
    if (k_msgq_put(&peripheral_command_msgq, &command, K_NO_WAIT) < 0) {
        LOG_WRN("Dropping command, queue full");
        return;
    }
    k_work_submit(&peripheral_command_work);
}

/* Single store, else reader mixes modifiers and indicators from two beacons. */
static atomic_t synced_hid_state;

uint8_t zmk_split_esb_hid_modifiers(void) {
    return esb_hid_state_modifiers((uint16_t)atomic_get(&synced_hid_state));
}

uint8_t zmk_split_esb_hid_indicators(void) {
    return esb_hid_state_indicators((uint16_t)atomic_get(&synced_hid_state));
}

void peripheral_hid_state_store(uint8_t modifiers, uint8_t indicators) {
    atomic_set(&synced_hid_state, esb_hid_state_pack(modifiers, indicators));
}
