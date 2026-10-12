// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Button held while the central has its own host reaches the dongle again after. */
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <dt-bindings/zmk/pointing.h>
#include <zmk/hid.h>

#include "mock.h"
#include "relay_host.h"

#define PAUSE_MS 100
#define RESUME_MS 200
#define VERDICT_MS 300

static atomic_t button_relayed;

void relay_host_observe_pointer(enum relay_host_phase phase,
                                const struct zmk_hid_mouse_report *report) {
    if (phase == RELAY_HOST_PHASE_RESUMED && relay_host_pointer_holds(report, MB1)) {
        atomic_set(&button_relayed, 1);
    }
}

void relay_host_scenario(void) {
    k_sleep(K_TIMEOUT_ABS_MS(PAUSE_MS));
    relay_host_pause();
    k_sleep(K_TIMEOUT_ABS_MS(RESUME_MS));
    relay_host_resume();
    k_sleep(K_TIMEOUT_ABS_MS(VERDICT_MS));
    bool relayed = atomic_get(&button_relayed) != 0;
    mock_check(relayed, "held button relayed again after the pause");
}
