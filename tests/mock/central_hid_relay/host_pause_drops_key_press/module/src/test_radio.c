// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Key pressed while the central has its own host never reaches the dongle. */
#include <stdbool.h>

#include <zephyr/kernel.h>

#include <dt-bindings/zmk/keys.h>
#include <zmk/hid.h>

#include "mock.h"
#include "relay_host.h"

#define PAUSE_MS 100
#define VERDICT_MS 250

void relay_host_observe_keyboard(enum relay_host_phase phase,
                                 const struct zmk_hid_keyboard_report *report) {
    ARG_UNUSED(phase);
    mock_check(relay_host_keyboard_released(report), "relay sends no key pressed while paused");
}

void relay_host_scenario(void) {
    k_sleep(K_TIMEOUT_ABS_MS(PAUSE_MS));
    relay_host_pause();
    k_sleep(K_TIMEOUT_ABS_MS(VERDICT_MS));
    bool pressed = relay_host_keyboard_holds(zmk_hid_get_keyboard_report(), A);
    mock_check(pressed, "key A pressed while paused");
}
