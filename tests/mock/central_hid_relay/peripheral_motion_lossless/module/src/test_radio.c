// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central relaying a lossless mouse peripheral's motion
 * to the dongle's own delivery code, behind a fake USB sink.
 * Mouse sends a delta every MOTION_PERIOD_MS, the dongle polls every RELAY_POLL_MS.
 * Exits 0 once the sink got every count the mouse sent, never a step back.
 * Exits 1 on motion short at the sink or a step back.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/hid.h>
#include <zmk/split/transport/types.h>

#include <zmk_split_esb_hid_relay.h>

#include <esb.h>

#include "central.h"
#include "esb_hid_relay_peripheral.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "esb_wire.h"
#include "hop.h"
#include "mock.h"

#define MOUSE_PIPE DT_PROP(DT_NODELABEL(mouse), pipe)
#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define POINTER_REG DT_REG_ADDR(DT_NODELABEL(split_pointer))
#define REPORT_ID_OFFSET 0
#define POLL_START_MS 20
#define RELAY_POLL_MS 2
#define MOTION_START_MS 100
#define MOTION_PERIOD_MS 4
#define MOTION_STEPS 100
#define SETTLE_MS 300
#define VERDICT_MS (MOTION_START_MS + MOTION_STEPS * MOTION_PERIOD_MS + SETTLE_MS)

static uint32_t steps_sent;
static int32_t sink_x;

int esb_write_payload(const struct esb_payload *payload) {
    if (payload->pipe == RELAY_PIPE) {
        esb_hid_relay_deliver(payload->data, payload->length);
    }
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
    return 0;
}

bool hop_pipe_heard(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return false;
}

static int usb_sink(const uint8_t *bytes, size_t length, bool skippable) {
    ARG_UNUSED(skippable);
    struct zmk_hid_mouse_report report = {0};
    if (length != sizeof(report) || bytes[REPORT_ID_OFFSET] != ZMK_HID_REPORT_ID_MOUSE) {
        return 0;
    }
    memcpy(&report, bytes, sizeof(report));
    if (report.body.d_x < 0) {
        printk("FAIL: sink stepped back by %d\n", report.body.d_x);
        exit(1);
    }
    sink_x += report.body.d_x;
    return 0;
}

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    esb_link_role_rx_done((uint8_t)BIT(RELAY_PIPE));
    k_work_reschedule(&relay_poll_work, K_MSEC(RELAY_POLL_MS));
}

static void send_delta(void) {
    const struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data.input_event = {.reg = POINTER_REG,
                             .type = INPUT_EV_REL,
                             .code = INPUT_REL_X,
                             .value = 1,
                             .sync = ESB_WIRE_INPUT_SYNC},
    };
    uint8_t wire[ESB_WIRE_MAX_EVENT_SIZE];
    size_t length = esb_wire_encode_event(wire, sizeof(wire), &event);
    mock_check(length != 0, "mouse delta encodes");
    central_ingest_packet(MOUSE_PIPE, wire, length);
}

static void motion_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(motion_work, motion_fn);

static void motion_fn(struct k_work *work) {
    ARG_UNUSED(work);
    send_delta();
    steps_sent++;
    if (steps_sent < MOTION_STEPS) {
        k_work_reschedule(&motion_work, K_MSEC(MOTION_PERIOD_MS));
    }
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (sink_x != MOTION_STEPS) {
        printk("FAIL: sink got %d of %d counts\n", (int)sink_x, MOTION_STEPS);
        exit(1);
    }
    printk("PASS: sink got all %d counts of the lossless mouse\n", (int)sink_x);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    mock_check(zmk_split_esb_hid_relay_register(usb_sink) == 0, "sink registers");
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_START_MS));
    k_work_reschedule(&motion_work, K_MSEC(MOTION_START_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
