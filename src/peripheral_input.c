// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Input split on a split peripheral. */

#include "peripheral_input.h"

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define HELD_ENTRY_VALID BIT(24)
#define HELD_ENTRY_REG_SHIFT 16

/* One word per held key, so the keepalive tick reads each entry whole without a lock. */
static atomic_t held_input_keys[PERIPHERAL_INPUT_HELD_KEYS_MAX];

static atomic_val_t held_entry_pack(uint8_t reg, uint16_t code) {
    return (atomic_val_t)(HELD_ENTRY_VALID | ((uint32_t)reg << HELD_ENTRY_REG_SHIFT) | code);
}

static struct esb_keepalive_held_key held_entry_unpack(atomic_val_t entry) {
    return (struct esb_keepalive_held_key){
        .reg = (uint8_t)((uint32_t)entry >> HELD_ENTRY_REG_SHIFT),
        .code = (uint16_t)entry,
    };
}

static void held_entry_add(atomic_val_t entry) {
    for (size_t index = 0; index < ARRAY_SIZE(held_input_keys); index++) {
        if (atomic_get(&held_input_keys[index]) == entry) {
            return;
        }
    }
    for (size_t index = 0; index < ARRAY_SIZE(held_input_keys); index++) {
        if (atomic_get(&held_input_keys[index]) == 0) {
            atomic_set(&held_input_keys[index], entry);
            return;
        }
    }
    LOG_WRN("Held input key table full, keepalive misses one");
}

static void held_entry_remove(atomic_val_t entry) {
    for (size_t index = 0; index < ARRAY_SIZE(held_input_keys); index++) {
        if (atomic_get(&held_input_keys[index]) == entry) {
            atomic_set(&held_input_keys[index], 0);
            return;
        }
    }
}

void peripheral_input_note_event(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT) {
        return;
    }
    if (event->data.input_event.type != INPUT_EV_KEY) {
        return;
    }
    atomic_val_t entry = held_entry_pack(event->data.input_event.reg, event->data.input_event.code);
    if (event->data.input_event.value != 0) {
        held_entry_add(entry);
    } else {
        held_entry_remove(entry);
    }
}

uint8_t peripheral_input_held_keys(struct esb_keepalive_held_key *out) {
    uint8_t count = 0;
    for (size_t index = 0; index < ARRAY_SIZE(held_input_keys); index++) {
        atomic_val_t entry = atomic_get(&held_input_keys[index]);
        if (entry != 0) {
            out[count] = held_entry_unpack(entry);
            count++;
        }
    }
    return count;
}
