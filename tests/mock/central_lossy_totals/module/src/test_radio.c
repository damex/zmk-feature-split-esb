// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central fed a lossy mouse's running totals.
 * Packets at 60 ms and 70 ms are lost, the keepalive at 75 ms comes before
 * a hop window of quiet, the one at 100 ms repairs the lost tail.
 * The keepalive at 121 ms trails resumed motion with an older snapshot.
 * Silence past peripheral-timeout-ms releases the mouse, which comes back with totals from zero.
 * Exits 0 once the input split device moved by exactly the mouse's motion, never backwards.
 * Exits 1 on a backward step or a wrong total at verdict.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/types.h>

#include <esb.h>

#include "central.h"
#include "esb_keepalive.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "esb_wire.h"
#include "hop.h"

#define PIPE DT_PROP(DT_NODELABEL(mouse), pipe)
#define POINTER_REG DT_REG_ADDR(DT_NODELABEL(split_pointer))
#define EXPECTED_X 27
#define EXPECTED_Y 19
#define VERDICT_MS 1300

enum step_kind {
    STEP_PACKET,
    STEP_KEEPALIVE,
    STEP_SILENCE,
    STEP_HEARD,
};

struct step {
    uint32_t at_ms;
    enum step_kind kind;
    int32_t x_total;
    int32_t y_total;
};

static const struct step steps[] = {
    {.at_ms = 50, .kind = STEP_PACKET, .x_total = 10, .y_total = 5},
    {.at_ms = 55, .kind = STEP_PACKET, .x_total = 13, .y_total = 9},
    {.at_ms = 65, .kind = STEP_PACKET, .x_total = 25, .y_total = 14},
    {.at_ms = 75, .kind = STEP_KEEPALIVE, .x_total = 30, .y_total = 20},
    {.at_ms = 100, .kind = STEP_KEEPALIVE, .x_total = 30, .y_total = 20},
    {.at_ms = 120, .kind = STEP_PACKET, .x_total = 35, .y_total = 22},
    {.at_ms = 121, .kind = STEP_KEEPALIVE, .x_total = 30, .y_total = 20},
    {.at_ms = 150, .kind = STEP_SILENCE},
    {.at_ms = 1100, .kind = STEP_HEARD},
    {.at_ms = 1101, .kind = STEP_PACKET, .x_total = 2, .y_total = 1},
    {.at_ms = 1106, .kind = STEP_PACKET, .x_total = 4, .y_total = 3},
};

static const uint8_t no_positions[ESB_KEEPALIVE_BITMAP_BYTES];
static atomic_t silent;
static atomic_t moved_x;
static atomic_t moved_y;
static size_t next_step;

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    ARG_UNUSED(callback);
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    return 0;
}

int esb_link_hfclk_acquire(void) {
    return 0;
}

void hop_boot_mask(void) {
}

uint32_t hop_pipe_quiet_ms(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return atomic_get(&silent) != 0 ? UINT32_MAX : 0;
}

bool hop_pipe_heard(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return true;
}

static void pointer_observer(struct input_event *event, void *user_data) {
    ARG_UNUSED(user_data);
    if (event->type != INPUT_EV_REL) {
        return;
    }
    if (event->value < 0) {
        printk("FAIL: central stepped axis 0x%02x back by %d\n", event->code, (int)event->value);
        exit(1);
    }
    if (event->code == INPUT_REL_X) {
        atomic_add(&moved_x, event->value);
    } else if (event->code == INPUT_REL_Y) {
        atomic_add(&moved_y, event->value);
    }
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(split_pointer)), pointer_observer, NULL);

static size_t encode_total(uint16_t code, int32_t total, bool sync, uint8_t *out, size_t room) {
    uint8_t sync_byte = ESB_WIRE_INPUT_TOTAL;
    if (sync) {
        sync_byte |= ESB_WIRE_INPUT_SYNC;
    }
    const struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data.input_event = {.reg = POINTER_REG,
                             .type = INPUT_EV_REL,
                             .code = code,
                             .value = total,
                             .sync = sync_byte},
    };
    size_t length = esb_wire_encode_event(out, room, &event);
    if (length == 0) {
        printk("FAIL: total does not encode\n");
        exit(1);
    }
    return length;
}

static void send_packet(const struct step *step) {
    uint8_t packet[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
    size_t length = encode_total(INPUT_REL_X, step->x_total, false, packet, sizeof(packet));
    length += encode_total(INPUT_REL_Y, step->y_total, true, &packet[length],
                           sizeof(packet) - length);
    central_ingest_packet(PIPE, packet, length);
}

static void send_keepalive(const struct step *step) {
    const struct esb_keepalive_motion_total motion[] = {
        {.reg = POINTER_REG, .code = INPUT_REL_X, .short_total = (int16_t)step->x_total},
        {.reg = POINTER_REG, .code = INPUT_REL_Y, .short_total = (int16_t)step->y_total},
    };
    const struct esb_keepalive_snapshot snapshot = {
        .state = ESB_KEEPALIVE_IDLE,
        .battery_level = ESB_KEEPALIVE_BATTERY_UNKNOWN,
        .position_bitmap = no_positions,
        .motion_totals = motion,
        .motion_count = ARRAY_SIZE(motion),
    };
    uint8_t keepalive[ESB_KEEPALIVE_LENGTH(0, ARRAY_SIZE(motion), 0)];
    size_t length = esb_keepalive_encode(keepalive, sizeof(keepalive), &snapshot);
    central_ingest_packet(PIPE, keepalive, length);
}

static void run_step(const struct step *step) {
    switch (step->kind) {
    case STEP_PACKET:
        send_packet(step);
        break;
    case STEP_KEEPALIVE:
        send_keepalive(step);
        break;
    case STEP_SILENCE:
        atomic_set(&silent, 1);
        break;
    case STEP_HEARD:
        atomic_set(&silent, 0);
        break;
    default:
        printk("FAIL: unknown step kind %d\n", (int)step->kind);
        exit(1);
    }
}

static void step_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(step_work, step_fn);

static void step_fn(struct k_work *work) {
    ARG_UNUSED(work);
    run_step(&steps[next_step]);
    next_step++;
    if (next_step < ARRAY_SIZE(steps)) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t at_ms = steps[next_step].at_ms;
        k_work_reschedule(&step_work, K_MSEC(at_ms > now_ms ? at_ms - now_ms : 0));
    }
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_val_t x = atomic_get(&moved_x);
    atomic_val_t y = atomic_get(&moved_y);
    if (x != EXPECTED_X || y != EXPECTED_Y) {
        printk("FAIL: central moved x %d y %d, mouse moved x %d y %d past its baselines\n",
               (int)x, (int)y, EXPECTED_X, EXPECTED_Y);
        exit(1);
    }
    printk("PASS: central moved x %d y %d through losses, a stale keepalive and a reboot\n",
           (int)x, (int)y);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&step_work, K_MSEC(steps[0].at_ms));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
