// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Motion while the central has its own host never reaches the dongle, not even after. */
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <zmk/hid.h>

#include "mock.h"
#include "relay_host.h"

#define ACTIVE_TOTAL_X 4
#define PAUSE_MS 100
#define RESUME_MS 240
#define VERDICT_MS 340

static atomic_t active_total_x;
static atomic_t resumed_report_sent;

void relay_host_observe_pointer(enum relay_host_phase phase,
                                const struct zmk_hid_mouse_report *report) {
    if (phase == RELAY_HOST_PHASE_ACTIVE) {
        atomic_set(&active_total_x, report->body.d_x);
        return;
    }
    bool still = report->body.d_x == ACTIVE_TOTAL_X;
    mock_check(still, "relay sends no motion from the pause on");
    if (phase == RELAY_HOST_PHASE_RESUMED) {
        atomic_set(&resumed_report_sent, 1);
    }
}

void relay_host_scenario(void) {
    k_sleep(K_TIMEOUT_ABS_MS(PAUSE_MS));
    bool relayed = atomic_get(&active_total_x) == ACTIVE_TOTAL_X;
    mock_check(relayed, "motion relayed before the pause");
    relay_host_pause();
    k_sleep(K_TIMEOUT_ABS_MS(RESUME_MS));
    relay_host_resume();
    k_sleep(K_TIMEOUT_ABS_MS(VERDICT_MS));
    bool sent = atomic_get(&resumed_report_sent) != 0;
    mock_check(sent, "pointer report sent after the pause");
}
