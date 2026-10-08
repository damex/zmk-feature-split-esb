// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link on a HID relay dongle receiving pointer reports.
 * Exits 0 once every report with motion reaches the sink, repeats included,
 * a button change does too, and a repeat without motion or button change does not.
 * Exits 1 on a missing, extra or wrong report.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/hid.h>

#include <zmk_split_esb_hid_relay.h>

#include "esb_link.h"
#include "hop.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define DELIVER_DELAY_MS 50
#define PRIMARY_BUTTON BIT(0)

struct pointer_step {
    int16_t d_x;
    uint8_t buttons;
    bool reaches_sink;
};

static const struct pointer_step steps[] = {
    {.d_x = 5, .buttons = 0, .reaches_sink = true},
    {.d_x = 5, .buttons = 0, .reaches_sink = true},
    {.d_x = 0, .buttons = PRIMARY_BUTTON, .reaches_sink = true},
    {.d_x = 0, .buttons = PRIMARY_BUTTON, .reaches_sink = false},
    {.d_x = -1, .buttons = PRIMARY_BUTTON, .reaches_sink = true},
};

static size_t step_index;
static size_t sink_hits;
static esb_link_rx_callback_t rx_callback;

int esb_link_init(esb_link_rx_callback_t callback) {
    rx_callback = callback;
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    return 0;
}

int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    ARG_UNUSED(data);
    ARG_UNUSED(length);
    ARG_UNUSED(ack);
    return 0;
}

void hop_restore(void) {
}

uint8_t hop_link_cost_x10(void) {
    return 0;
}

static int sink(const uint8_t *bytes, size_t length) {
    const struct pointer_step *step = &steps[step_index];
    if (!step->reaches_sink) {
        printk("FAIL: step %u, a repeat without motion reached the sink\n",
               (unsigned int)(step_index + 1));
        exit(1);
    }
    struct zmk_hid_mouse_report report = {0};
    if (length != sizeof(report)) {
        printk("FAIL: step %u reached the sink as %u bytes\n", (unsigned int)(step_index + 1),
               (unsigned int)length);
        exit(1);
    }
    memcpy(&report, bytes, sizeof(report));
    bool matches = report.report_id == ZMK_HID_REPORT_ID_MOUSE && report.body.d_x == step->d_x &&
                   report.body.buttons == step->buttons;
    if (!matches) {
        printk("FAIL: step %u reached the sink as id 0x%02x, d_x %d, buttons 0x%02x\n",
               (unsigned int)(step_index + 1), report.report_id, report.body.d_x,
               report.body.buttons);
        exit(1);
    }
    sink_hits++;
    return 0;
}

static void deliver_step(size_t index) {
    step_index = index;
    const struct pointer_step *step = &steps[index];
    struct zmk_hid_mouse_report report = {
        .report_id = ZMK_HID_REPORT_ID_MOUSE,
        .body = {.buttons = step->buttons, .d_x = step->d_x},
    };
    size_t hits_before = sink_hits;
    rx_callback(SELF_PIPE, (const uint8_t *)&report, sizeof(report));
    if (step->reaches_sink && sink_hits != hits_before + 1) {
        printk("FAIL: step %u never reached the sink\n", (unsigned int)(index + 1));
        exit(1);
    }
}

static void deliver_fn(struct k_work *work) {
    ARG_UNUSED(work);
    for (size_t index = 0; index < ARRAY_SIZE(steps); index++) {
        deliver_step(index);
    }
    printk("PASS: %u pointer reports reached the sink, repeats with motion included\n",
           (unsigned int)sink_hits);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(deliver_work, deliver_fn);

static int test_radio_init(void) {
    int error = zmk_split_esb_hid_relay_register(sink);
    if (error != 0) {
        printk("FAIL: sink registration returned %d\n", error);
        exit(1);
    }
    k_work_reschedule(&deliver_work, K_MSEC(DELIVER_DELAY_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
