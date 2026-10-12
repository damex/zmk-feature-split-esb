// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Button press queued as the central gets its own host goes out released to the dongle. */
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <dt-bindings/zmk/pointing.h>
#include <zmk/hid.h>

#include "esb_hid_relay_central.h"
#include "mock.h"
#include "relay_host.h"

#define PAUSE_MS 100
#define VERDICT_MS 200

static atomic_t paused_report_sent;

void relay_host_observe_pointer(enum relay_host_phase phase,
                                const struct zmk_hid_mouse_report *report) {
    bool released = report->body.buttons == 0;
    mock_check(released, "relay sends no button press queued at the pause");
    if (phase == RELAY_HOST_PHASE_PAUSED) {
        atomic_set(&paused_report_sent, 1);
    }
}

void relay_host_scenario(void) {
    k_sleep(K_TIMEOUT_ABS_MS(PAUSE_MS));
    const struct zmk_hid_mouse_report press = {
        .report_id = ZMK_HID_REPORT_ID_MOUSE,
        .body = {.buttons = (zmk_mouse_button_flags_t)(MB1 | MB2)},
    };
    esb_hid_relay_stage(&press, sizeof(press));
    relay_host_pause();
    k_sleep(K_TIMEOUT_ABS_MS(VERDICT_MS));
    bool sent = atomic_get(&paused_report_sent) != 0;
    mock_check(sent, "pointer report sent while paused");
}
