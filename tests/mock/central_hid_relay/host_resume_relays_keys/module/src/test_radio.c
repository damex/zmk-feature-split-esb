// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Key pressed after the central loses its own host reaches the dongle again. */
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <dt-bindings/zmk/keys.h>
#include <zmk/hid.h>

#include "mock.h"
#include "relay_host.h"

#define PAUSE_MS 100
#define RESUME_MS 200
#define VERDICT_MS 350

static atomic_t key_relayed;

void relay_host_observe_keyboard(enum relay_host_phase phase,
                                 const struct zmk_hid_keyboard_report *report) {
    if (phase == RELAY_HOST_PHASE_RESUMED && relay_host_keyboard_holds(report, A)) {
        atomic_set(&key_relayed, 1);
    }
}

void relay_host_scenario(void) {
    k_sleep(K_TIMEOUT_ABS_MS(PAUSE_MS));
    relay_host_pause();
    k_sleep(K_TIMEOUT_ABS_MS(RESUME_MS));
    relay_host_resume();
    k_sleep(K_TIMEOUT_ABS_MS(VERDICT_MS));
    bool relayed = atomic_get(&key_relayed) != 0;
    mock_check(relayed, "key A relayed after the pause");
}
