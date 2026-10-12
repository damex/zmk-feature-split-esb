// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Key held when the central gets its own host goes out released to the dongle. */
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <dt-bindings/zmk/keys.h>
#include <zmk/hid.h>

#include "mock.h"
#include "relay_host.h"

#define PAUSE_MS 100
#define VERDICT_MS 200

static atomic_t key_relayed;
static atomic_t release_sent;

void relay_host_observe_keyboard(enum relay_host_phase phase,
                                 const struct zmk_hid_keyboard_report *report) {
    if (phase == RELAY_HOST_PHASE_ACTIVE) {
        if (relay_host_keyboard_holds(report, A)) {
            atomic_set(&key_relayed, 1);
        }
        return;
    }
    mock_check(relay_host_keyboard_released(report), "paused relay sends no held key");
    atomic_set(&release_sent, 1);
}

void relay_host_scenario(void) {
    k_sleep(K_TIMEOUT_ABS_MS(PAUSE_MS));
    bool relayed = atomic_get(&key_relayed) != 0;
    mock_check(relayed, "key A relayed before the pause");
    relay_host_pause();
    k_sleep(K_TIMEOUT_ABS_MS(VERDICT_MS));
    bool released = atomic_get(&release_sent) != 0;
    mock_check(released, "released keyboard report sent while paused");
}
