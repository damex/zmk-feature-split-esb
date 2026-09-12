// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

void peripheral_set_transport(const struct zmk_split_transport_peripheral *instance);

int peripheral_encode_event(const struct zmk_split_transport_peripheral_event *event,
                            uint8_t *wire, size_t wire_size);

void peripheral_note_key_event(uint8_t position, bool pressed);
void peripheral_sensor_event_to_total(struct zmk_split_transport_peripheral_event *event);

const uint8_t *peripheral_pressed_bitmap(void);
uint8_t peripheral_battery_level(void);
const int64_t *peripheral_sensor_totals(void);
uint8_t peripheral_sensor_count(void);

void peripheral_deliver_command(const uint8_t *bytes, size_t length);
void peripheral_hid_state_store(uint8_t modifiers, uint8_t indicators);
