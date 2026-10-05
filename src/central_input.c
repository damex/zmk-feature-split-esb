// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Input split on a split central. */

#include "central_input.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing/input_split.h>

#include "esb_keepalive.h"
#include "esb_link_internal.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define CENTRAL_INPUT_REG_MAX 32

/* Written on the RX thread, read by the staleness tick: single writer per slot,
 * aligned loads, no lock needed. */
static uint32_t pipe_seen_input_regs[ESB_LINK_PIPE_MAX];

struct tracked_input_key {
    bool held;
    struct esb_keepalive_held_key key;
};

static struct tracked_input_key tracked_input_keys[ESB_LINK_PIPE_MAX][CENTRAL_INPUT_HELD_KEYS_MAX];

/* Staleness tick only flags, so tracked input keys keep one writer. */
static ATOMIC_DEFINE(input_keys_reset_pipes, ESB_LINK_PIPE_MAX);

static void note_input_reg(uint8_t pipe, uint8_t reg) {
    if (reg < CENTRAL_INPUT_REG_MAX) {
        pipe_seen_input_regs[pipe] |= BIT(reg);
    }
}

static bool input_keys_equal(struct esb_keepalive_held_key left,
                             struct esb_keepalive_held_key right) {
    return left.reg == right.reg && left.code == right.code;
}

static struct tracked_input_key *find_tracked_input_key(uint8_t pipe,
                                                        struct esb_keepalive_held_key key) {
    for (size_t index = 0; index < ARRAY_SIZE(tracked_input_keys[pipe]); index++) {
        struct tracked_input_key *slot = &tracked_input_keys[pipe][index];
        if (slot->held && input_keys_equal(slot->key, key)) {
            return slot;
        }
    }
    return NULL;
}

static int track_input_key(uint8_t pipe, struct esb_keepalive_held_key key) {
    for (size_t index = 0; index < ARRAY_SIZE(tracked_input_keys[pipe]); index++) {
        struct tracked_input_key *slot = &tracked_input_keys[pipe][index];
        if (!slot->held) {
            *slot = (struct tracked_input_key){.held = true, .key = key};
            return 0;
        }
    }
    return -ENOMEM;
}

static void untrack_input_key(uint8_t pipe, struct esb_keepalive_held_key key) {
    struct tracked_input_key *slot = find_tracked_input_key(pipe, key);
    if (slot != NULL) {
        slot->held = false;
    }
}

static void apply_input_keys_reset(uint8_t pipe) {
    if (!atomic_test_and_clear_bit(input_keys_reset_pipes, pipe)) {
        return;
    }
    for (size_t index = 0; index < ARRAY_SIZE(tracked_input_keys[pipe]); index++) {
        tracked_input_keys[pipe][index].held = false;
    }
}

void central_input_deliver_event(const struct zmk_split_transport_central *transport, uint8_t pipe,
                                 const struct zmk_split_transport_peripheral_event *event) {
    if (pipe >= ESB_LINK_PIPE_MAX) {
        (void)zmk_split_transport_central_peripheral_event_handler(transport, pipe, *event);
        return;
    }
    apply_input_keys_reset(pipe);
    note_input_reg(pipe, event->data.input_event.reg);
    if (event->data.input_event.type == INPUT_EV_KEY) {
        struct esb_keepalive_held_key key = {
            .reg = event->data.input_event.reg,
            .code = event->data.input_event.code,
        };
        bool pressed = event->data.input_event.value != 0;
        /* Keepalive taken between peripheral note and send lands ahead of its press. */
        if (pressed && find_tracked_input_key(pipe, key) != NULL) {
            LOG_DBG("Dropping repeated press of input code %u from %u", (unsigned int)key.code,
                    (unsigned int)pipe);
            return;
        }
        if (!pressed) {
            untrack_input_key(pipe, key);
        } else if (track_input_key(pipe, key) < 0) {
            LOG_WRN("Input key table full, code %u from %u untracked", (unsigned int)key.code,
                    (unsigned int)pipe);
        }
    }
    (void)zmk_split_transport_central_peripheral_event_handler(transport, pipe, *event);
}

static void replay_input_key(const struct zmk_split_transport_central *transport, uint8_t pipe,
                             struct esb_keepalive_held_key key, bool pressed) {
    LOG_WRN("Reconcile lost: input code %u %s from %u", (unsigned int)key.code,
            pressed ? "press" : "release", (unsigned int)pipe);
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data = {.input_event = {
                     .reg = key.reg,
                     .type = INPUT_EV_KEY,
                     .code = key.code,
                     .value = pressed,
                     .sync = true,
                 }},
    };
    (void)zmk_split_transport_central_peripheral_event_handler(transport, pipe, event);
}

static bool keepalive_holds_input_key(const uint8_t *keepalive,
                                      struct esb_keepalive_held_key key) {
    uint8_t held_count = esb_keepalive_held_count(keepalive);
    for (uint8_t index = 0; index < held_count; index++) {
        if (input_keys_equal(esb_keepalive_held_key_at(keepalive, index), key)) {
            return true;
        }
    }
    return false;
}

static void release_unheld_input_keys(const struct zmk_split_transport_central *transport,
                                      uint8_t pipe, const uint8_t *keepalive) {
    for (size_t index = 0; index < ARRAY_SIZE(tracked_input_keys[pipe]); index++) {
        struct tracked_input_key *slot = &tracked_input_keys[pipe][index];
        if (!slot->held || keepalive_holds_input_key(keepalive, slot->key)) {
            continue;
        }
        slot->held = false;
        replay_input_key(transport, pipe, slot->key, false);
    }
}

static void press_held_input_keys(const struct zmk_split_transport_central *transport,
                                  uint8_t pipe, const uint8_t *keepalive) {
    uint8_t held_count = esb_keepalive_held_count(keepalive);
    for (uint8_t index = 0; index < held_count; index++) {
        struct esb_keepalive_held_key key = esb_keepalive_held_key_at(keepalive, index);
        if (find_tracked_input_key(pipe, key) != NULL) {
            continue;
        }
        if (track_input_key(pipe, key) < 0) {
            LOG_DBG("Input key table full, code %u from %u not replayed", (unsigned int)key.code,
                    (unsigned int)pipe);
            continue;
        }
        note_input_reg(pipe, key.reg);
        replay_input_key(transport, pipe, key, true);
    }
}

void central_input_reconcile(const struct zmk_split_transport_central *transport, uint8_t pipe,
                             const uint8_t *keepalive) {
    if (pipe >= ESB_LINK_PIPE_MAX) {
        return;
    }
    apply_input_keys_reset(pipe);
    release_unheld_input_keys(transport, pipe, keepalive);
    press_held_input_keys(transport, pipe, keepalive);
}

void central_input_release_pipe(uint8_t pipe) {
    if (pipe >= ESB_LINK_PIPE_MAX) {
        return;
    }
    for (uint8_t reg = 0; reg < CENTRAL_INPUT_REG_MAX; reg++) {
        if ((pipe_seen_input_regs[pipe] & BIT(reg)) != 0) {
            zmk_input_split_peripheral_disconnected(reg);
        }
    }
    atomic_set_bit(input_keys_reset_pipes, pipe);
}
