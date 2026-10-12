// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Dongle pointer step runner, with ESB link and hop fakes. */

#include "dongle_pointer.h"

#include <errno.h>
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

static void check_sink_report(const struct dongle_pointer_step *step,
                              const uint8_t *bytes,
                              size_t length) {
    struct zmk_hid_mouse_report report = {0};
    if (length != sizeof(report)) {
        printk("FAIL: step %u reached the sink as %u bytes\n",
               (unsigned int)(step_index + 1),
               (unsigned int)length);
        exit(1);
    }
    memcpy(&report, bytes, sizeof(report));
    bool matches = report.report_id == ZMK_HID_REPORT_ID_MOUSE && report.body.d_x == step->d_x &&
                   report.body.d_y == step->d_y && report.body.buttons == step->buttons;
    if (!matches) {
        printk("FAIL: step %u reached the sink as id 0x%02x, d_x %d, d_y %d, buttons 0x%02x\n",
               (unsigned int)(step_index + 1),
               report.report_id,
               report.body.d_x,
               report.body.d_y,
               report.body.buttons);
        exit(1);
    }
}

static int sink(const uint8_t *bytes, size_t length, bool skippable) {
    const struct dongle_pointer_step *step = &dongle_pointer_steps[step_index];
    if (skippable != step->skippable) {
        printk("FAIL: step %u reached the sink with skippable %d\n",
               (unsigned int)(step_index + 1),
               (int)skippable);
        exit(1);
    }
    if (step->sink_busy) {
        return -EBUSY;
    }
    if (!step->reaches_sink) {
        printk("FAIL: step %u reached the sink without news\n", (unsigned int)(step_index + 1));
        exit(1);
    }
    check_sink_report(step, bytes, length);
    sink_hits++;
    return 0;
}

static void deliver_step(size_t index) {
    step_index = index;
    const struct dongle_pointer_step *step = &dongle_pointer_steps[index];
    if (step->silence_before_ms != 0) {
        k_msleep((int32_t)step->silence_before_ms);
    }
    struct zmk_hid_mouse_report report = {
        .report_id = ZMK_HID_REPORT_ID_MOUSE,
        .body = {.buttons = step->buttons, .d_x = step->total_x, .d_y = step->total_y},
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
    for (size_t index = 0; index < dongle_pointer_step_count; index++) {
        deliver_step(index);
    }
    printk("PASS: all %u steps as expected, sink hits %u\n",
           (unsigned int)dongle_pointer_step_count,
           (unsigned int)sink_hits);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(deliver_work, deliver_fn);

static int dongle_pointer_init(void) {
    int error = zmk_split_esb_hid_relay_register(sink);
    if (error != 0) {
        printk("FAIL: sink registration returned %d\n", error);
        exit(1);
    }
    k_work_reschedule(&deliver_work, K_MSEC(DELIVER_DELAY_MS));
    return 0;
}
SYS_INIT(dongle_pointer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
