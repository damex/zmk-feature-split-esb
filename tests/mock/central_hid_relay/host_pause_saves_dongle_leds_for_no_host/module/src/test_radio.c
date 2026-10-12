// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Dongle host LEDs while the central has its own host become ZMK's state for no host. */
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/endpoints_types.h>
#include <zmk/hid_indicators.h>

#include "esb_hid_state.h"
#include "mock.h"
#include "relay_host.h"

#define PAUSE_MS 100

void relay_host_scenario(void) {
    k_sleep(K_TIMEOUT_ABS_MS(PAUSE_MS));
    relay_host_pause();
    const struct esb_host_indicators packet = {
        .tag = ESB_HOST_INDICATORS_TAG,
        .indicators = HID_INDICATOR_CAPS_LOCK,
    };
    relay_host_receive((const uint8_t *)&packet, sizeof(packet));
    const struct zmk_endpoint_instance no_host = {.transport = ZMK_TRANSPORT_NONE};
    bool saved = zmk_hid_indicators_get_profile(no_host) == HID_INDICATOR_CAPS_LOCK;
    mock_check(saved, "dongle host LEDs saved as ZMK's state for no host");
}
