// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Peripheral traffic tick: sends what is due, then checks the central's ACK FIFO. */
#define DT_DRV_COMPAT zmk_split_esb

#include "peripheral_traffic.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/types.h>

#include <esb.h>

#include "esb_hid_state.h"
#include "esb_keepalive.h"
#include "esb_wire.h"
#include "mock.h"
#include "mock_esb_ack_fifo.h"

#define LEFT_PIPE DT_PROP(DT_NODELABEL(left), pipe)
#define MOUSE_PIPE DT_PROP(DT_NODELABEL(mouse), pipe)
#define RELAY_NODE DT_NODELABEL(relay)
#define PIPE_COUNT DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals))
#define POINTER_REG DT_REG_ADDR(DT_NODELABEL(split_pointer))
#define HOP_WINDOW_MS DT_INST_PROP(0, hop_window_ms)
#define IDLE_KEEPALIVE_MS DT_INST_PROP(0, idle_keepalive_ms)
#define LINK_COST_ONE_ATTEMPT_X10 10
#define SHIFT_POSITION 0
#define LETTER_POSITION 1
#define TYPING_STEP_MS 25
#define TRAFFIC_START_MS 50
#define TRAFFIC_RUN_MS 3000
#define TRAFFIC_THREAD_STACK_SIZE 2048
#define TRAFFIC_THREAD_PRIORITY 7

static const bool relay_present = DT_NODE_EXISTS(RELAY_NODE);
static const uint8_t relay_pipe =
    COND_CODE_1(DT_NODE_EXISTS(RELAY_NODE), (DT_PROP(RELAY_NODE, pipe)), (0));
static const uint32_t relay_poll_ms =
    COND_CODE_1(DT_NODE_EXISTS(RELAY_NODE), (CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS), (1));

struct typing_step {
    uint8_t position;
    bool pressed;
};

static const struct typing_step typing_cycle[] = {
    {.position = SHIFT_POSITION, .pressed = true},
    {.position = LETTER_POSITION, .pressed = true},
    {.position = LETTER_POSITION, .pressed = false},
    {.position = SHIFT_POSITION, .pressed = false},
};

static const uint8_t no_positions[ESB_KEEPALIVE_BITMAP_BYTES];
static uint8_t left_positions[ESB_KEEPALIVE_BITMAP_BYTES];
static size_t typing_index;
static int32_t mouse_total;

static void receive(uint8_t pipe, const uint8_t *data, size_t length, bool ack) {
    mock_check(length <= CONFIG_ESB_MAX_PAYLOAD_LENGTH, "traffic packet fits one ESB payload");
    struct esb_payload payload = {.pipe = pipe, .length = (uint8_t)length, .noack = 1};
    if (ack) {
        payload.noack = 0;
    }
    memcpy(payload.data, data, length);
    mock_esb_ack_fifo_receive(&payload);
}

static void send_event(uint8_t pipe,
                       const struct zmk_split_transport_peripheral_event *event,
                       bool ack) {
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    size_t length = esb_wire_encode_event(wire, sizeof(wire), event);
    mock_check(length != 0, "traffic event encodes");
    receive(pipe, wire, length, ack);
}

static void send_keepalive(uint8_t pipe,
                           const uint8_t *position_bitmap,
                           const struct esb_keepalive_motion_total *motion_totals,
                           uint8_t motion_count) {
    const struct esb_keepalive_snapshot snapshot = {
        .state = ESB_KEEPALIVE_IDLE,
        .link_cost_x10 = LINK_COST_ONE_ATTEMPT_X10,
        .battery_level = ESB_KEEPALIVE_BATTERY_UNKNOWN,
        .position_bitmap = position_bitmap,
        .motion_totals = motion_totals,
        .motion_count = motion_count,
    };
    uint8_t keepalive[ESB_KEEPALIVE_LENGTH(0, 1, 0)];
    size_t length = esb_keepalive_encode(keepalive, sizeof(keepalive), &snapshot);
    mock_check(length != 0, "traffic keepalive encodes");
    receive(pipe, keepalive, length, true);
}

static void send_left_key(uint8_t position, bool pressed) {
    const struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = position, .pressed = pressed},
    };
    esb_keepalive_bitmap_set(left_positions, position, pressed);
    send_event(LEFT_PIPE, &event, true);
}

static void type_next_step(void) {
    const struct typing_step *step = &typing_cycle[typing_index];
    send_left_key(step->position, step->pressed);
    typing_index = (typing_index + 1) % ARRAY_SIZE(typing_cycle);
}

static void send_left_keepalive(void) {
    send_keepalive(LEFT_PIPE, left_positions, NULL, 0);
}

static void send_mouse_motion(void) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data.input_event = {.reg = POINTER_REG,
                             .type = INPUT_EV_REL,
                             .code = INPUT_REL_X,
                             .value = 1,
                             .sync = ESB_WIRE_INPUT_SYNC},
    };
    mouse_total++;
    if (peripheral_traffic_mode.mouse_lossy) {
        event.data.input_event.value = mouse_total;
        event.data.input_event.sync = ESB_WIRE_INPUT_SYNC | ESB_WIRE_INPUT_TOTAL;
    }
    send_event(MOUSE_PIPE, &event, !peripheral_traffic_mode.mouse_lossy);
}

static void send_mouse_keepalive(void) {
    if (!peripheral_traffic_mode.mouse_lossy) {
        send_keepalive(MOUSE_PIPE, no_positions, NULL, 0);
        return;
    }
    const struct esb_keepalive_motion_total motion_total = {
        .reg = POINTER_REG,
        .code = INPUT_REL_X,
        .short_total = (int16_t)(uint16_t)mouse_total,
    };
    send_keepalive(MOUSE_PIPE, no_positions, &motion_total, 1);
}

static void send_relay_poll(void) {
    const struct esb_relay_poll poll = {.tag = ESB_RELAY_POLL_TAG};
    receive(relay_pipe, (const uint8_t *)&poll, sizeof(poll), true);
}

static uint32_t keepalive_period_ms(bool active) {
    if (active) {
        return HOP_WINDOW_MS;
    }
    return IDLE_KEEPALIVE_MS;
}

static bool due(uint32_t tick_ms, uint32_t period_ms) {
    return tick_ms % period_ms == 0;
}

static void send_due(uint32_t tick_ms) {
    if (peripheral_traffic_mode.mouse_moves) {
        send_mouse_motion();
    }
    if (relay_present && due(tick_ms, relay_poll_ms)) {
        send_relay_poll();
    }
    if (peripheral_traffic_mode.left_types && due(tick_ms, TYPING_STEP_MS)) {
        type_next_step();
    }
    if (due(tick_ms, keepalive_period_ms(peripheral_traffic_mode.left_types))) {
        send_left_keepalive();
    }
    if (due(tick_ms, keepalive_period_ms(peripheral_traffic_mode.mouse_moves))) {
        send_mouse_keepalive();
    }
}

static void check_ack_fifo(uint32_t tick_ms) {
    if (mock_esb_ack_fifo_full_refusals() == 0) {
        return;
    }
    printk("FAIL: central ACK FIFO full at %u ms, queued per pipe:", (unsigned int)tick_ms);
    for (uint8_t pipe = 0; pipe < PIPE_COUNT; pipe++) {
        printk(" %u", (unsigned int)mock_esb_ack_fifo_queued(pipe));
    }
    printk("\n");
    exit(1);
}

static void traffic_thread_fn(void *unused_a, void *unused_b, void *unused_c) {
    ARG_UNUSED(unused_a);
    ARG_UNUSED(unused_b);
    ARG_UNUSED(unused_c);
    for (uint32_t tick_ms = 0; tick_ms < TRAFFIC_RUN_MS; tick_ms++) {
        send_due(tick_ms);
        check_ack_fifo(tick_ms);
        k_msleep(1);
    }
    printk("PASS: central ACK FIFO took every write for %u ms, peak %u of %u slots\n",
           (unsigned int)TRAFFIC_RUN_MS,
           (unsigned int)mock_esb_ack_fifo_peak_slots(),
           (unsigned int)CONFIG_ESB_TX_FIFO_SIZE);
    exit(0);
}

K_THREAD_DEFINE(peripheral_traffic_thread,
                TRAFFIC_THREAD_STACK_SIZE,
                traffic_thread_fn,
                NULL,
                NULL,
                NULL,
                TRAFFIC_THREAD_PRIORITY,
                0,
                TRAFFIC_START_MS);
