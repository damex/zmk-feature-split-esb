// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Relay host harness, with ESB link core and hop fakes. */

#include "relay_host.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <zmk/endpoints_types.h>
#include <zmk/events/endpoint_changed.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define REPORT_ID_OFFSET 0
#define POLL_START_MS 20
#define THREAD_STACK_SIZE 2048
#define THREAD_PRIORITY K_PRIO_COOP(7)

static atomic_t current_phase = ATOMIC_INIT(RELAY_HOST_PHASE_ACTIVE);
static esb_link_rx_callback_t rx_callback;

__weak void relay_host_observe_keyboard(enum relay_host_phase phase,
                                        const struct zmk_hid_keyboard_report *report) {
    ARG_UNUSED(phase);
    ARG_UNUSED(report);
}

__weak void relay_host_observe_pointer(enum relay_host_phase phase,
                                       const struct zmk_hid_mouse_report *report) {
    ARG_UNUSED(phase);
    ARG_UNUSED(report);
}

static size_t observe_report(const uint8_t *bytes, size_t room) {
    enum relay_host_phase phase = (enum relay_host_phase)atomic_get(&current_phase);
    uint8_t report_id = bytes[REPORT_ID_OFFSET];
    if (report_id == ZMK_HID_REPORT_ID_KEYBOARD && room >= sizeof(struct zmk_hid_keyboard_report)) {
        struct zmk_hid_keyboard_report report = {0};
        memcpy(&report, bytes, sizeof(report));
        relay_host_observe_keyboard(phase, &report);
        return sizeof(report);
    }
    if (report_id == ZMK_HID_REPORT_ID_CONSUMER && room >= sizeof(struct zmk_hid_consumer_report)) {
        return sizeof(struct zmk_hid_consumer_report);
    }
    if (report_id == ZMK_HID_REPORT_ID_MOUSE && room >= sizeof(struct zmk_hid_mouse_report)) {
        struct zmk_hid_mouse_report report = {0};
        memcpy(&report, bytes, sizeof(report));
        relay_host_observe_pointer(phase, &report);
        return sizeof(report);
    }
    printk("FAIL: relay reply holds a report that is not whole, id 0x%02x\n", report_id);
    exit(1);
}

int esb_write_payload(const struct esb_payload *payload) {
    size_t offset = 0;
    while (offset < payload->length) {
        offset += observe_report(&payload->data[offset], payload->length - offset);
    }
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    rx_callback = callback;
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

static void raise_host(enum relay_host_phase phase, enum zmk_transport transport) {
    atomic_set(&current_phase, (atomic_val_t)phase);
    struct zmk_endpoint_changed changed = {.endpoint = {.transport = transport}};
    raise_zmk_endpoint_changed(changed);
}

void relay_host_pause(void) {
    raise_host(RELAY_HOST_PHASE_PAUSED, ZMK_TRANSPORT_USB);
}

void relay_host_resume(void) {
    raise_host(RELAY_HOST_PHASE_RESUMED, ZMK_TRANSPORT_NONE);
}

void relay_host_receive(const uint8_t *data, size_t length) {
    rx_callback(RELAY_PIPE, data, length);
}

bool relay_host_keyboard_holds(const struct zmk_hid_keyboard_report *report, uint32_t usage) {
    for (size_t index = 0; index < ARRAY_SIZE(report->body.keys); index++) {
        if (report->body.keys[index] == ZMK_HID_USAGE_ID(usage)) {
            return true;
        }
    }
    return false;
}

bool relay_host_keyboard_released(const struct zmk_hid_keyboard_report *report) {
    static const struct zmk_hid_keyboard_report_body released = {0};
    return memcmp(&report->body, &released, sizeof(released)) == 0;
}

bool relay_host_pointer_holds(const struct zmk_hid_mouse_report *report,
                              zmk_mouse_button_flags_t button) {
    return (report->body.buttons & button) != 0;
}

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    esb_link_role_rx_done((uint8_t)BIT(RELAY_PIPE));
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
}

static void relay_host_thread_fn(void *unused_a, void *unused_b, void *unused_c) {
    ARG_UNUSED(unused_a);
    ARG_UNUSED(unused_b);
    ARG_UNUSED(unused_c);
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_START_MS));
    relay_host_scenario();
    printk("PASS: every relay host check held\n");
    exit(0);
}

K_THREAD_DEFINE(relay_host_thread,
                THREAD_STACK_SIZE,
                relay_host_thread_fn,
                NULL,
                NULL,
                NULL,
                THREAD_PRIORITY,
                0,
                0);
