// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Button held when the central gets its own host goes out released to the dongle. */
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <dt-bindings/zmk/pointing.h>
#include <zmk/hid.h>

#include "mock.h"
#include "relay_host.h"

#define PAUSE_MS 100
#define VERDICT_MS 200

static atomic_t button_relayed;
static atomic_t release_sent;

void relay_host_observe_pointer(enum relay_host_phase phase,
                                const struct zmk_hid_mouse_report *report) {
    if (phase == RELAY_HOST_PHASE_ACTIVE) {
        if (relay_host_pointer_holds(report, MB1)) {
            atomic_set(&button_relayed, 1);
        }
        return;
    }
    bool released = report->body.buttons == 0;
    mock_check(released, "paused relay sends no held button");
    atomic_set(&release_sent, 1);
}

void relay_host_scenario(void) {
    k_sleep(K_TIMEOUT_ABS_MS(PAUSE_MS));
    bool relayed = atomic_get(&button_relayed) != 0;
    mock_check(relayed, "button relayed before the pause");
    relay_host_pause();
    k_sleep(K_TIMEOUT_ABS_MS(VERDICT_MS));
    bool sent = atomic_get(&release_sent) != 0;
    mock_check(sent, "button release sent while paused");
}
