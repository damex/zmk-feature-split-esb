// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Relay host harness on a real central with a relay dongle: the relay pipe polled,
 * each reply split into reports for the case's observers.
 * A case defines relay_host_scenario() and the observers it needs.
 * Exits 0 once the scenario returns, a failed check exits 1.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/hid.h>

enum relay_host_phase {
    RELAY_HOST_PHASE_ACTIVE,
    RELAY_HOST_PHASE_PAUSED,
    RELAY_HOST_PHASE_RESUMED,
};

/* Cooperative thread, sleeping between steps is fine.
 * No relay poll runs between two steps without a sleep. */
void relay_host_scenario(void);

/* Relay poll context, one call per report in the reply. Default: ignore. */
void relay_host_observe_keyboard(enum relay_host_phase phase,
                                 const struct zmk_hid_keyboard_report *report);
void relay_host_observe_pointer(enum relay_host_phase phase,
                                const struct zmk_hid_mouse_report *report);

/* Central gets its own USB host. */
void relay_host_pause(void);

/* Central loses its own USB host. */
void relay_host_resume(void);

/* Packet from the dongle on the relay pipe. */
void relay_host_receive(const uint8_t *data, size_t length);

bool relay_host_keyboard_holds(const struct zmk_hid_keyboard_report *report, uint32_t usage);
bool relay_host_keyboard_released(const struct zmk_hid_keyboard_report *report);
bool relay_host_pointer_holds(const struct zmk_hid_mouse_report *report,
                              zmk_mouse_button_flags_t button);
