// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HFXO for the ESB radio through nRF clock control. */

#include <errno.h>
#include <stdbool.h>

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/onoff.h>

#include "esb_link_internal.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static int hfclk_request(void) {
    struct onoff_manager *manager = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    struct onoff_client client;
    sys_notify_init_spinwait(&client.notify);
    int error = onoff_request(manager, &client);
    if (error < 0) {
        return error;
    }
    int result;
    while (sys_notify_fetch_result(&client.notify, &result) == -EAGAIN) {
    }
    return result;
}

static K_MUTEX_DEFINE(hfclk_mutex);
static bool hfclk_held;

int esb_link_hfclk_acquire(void) {
    k_mutex_lock(&hfclk_mutex, K_FOREVER);
    int error = 0;
    if (!hfclk_held) {
        error = hfclk_request();
        if (error == 0) {
            hfclk_held = true;
        }
    }
    k_mutex_unlock(&hfclk_mutex);
    return error;
}

void esb_link_hfclk_release(void) {
    k_mutex_lock(&hfclk_mutex, K_FOREVER);
    if (hfclk_held) {
        struct onoff_manager *manager =
            z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
        int release_error = onoff_release(manager);
        if (release_error < 0) {
            LOG_DBG("onoff_release returned %d", release_error);
        }
        hfclk_held = false;
    }
    k_mutex_unlock(&hfclk_mutex);
}
