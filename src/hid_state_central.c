// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Central half of HID-state sync: modifiers and indicators to peripherals over reverse channel.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

#if defined(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
#endif

#include <zmk_split_esb.h>

#include "esb_link_internal.h"
#include "hop.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static uint8_t sent_modifiers[ESB_LINK_PIPE_MAX];
static uint8_t sent_indicators[ESB_LINK_PIPE_MAX];
static bool sent_known[ESB_LINK_PIPE_MAX];

static uint8_t hid_state_indicators(void) {
#if defined(CONFIG_ZMK_HID_INDICATORS)
    return (uint8_t)zmk_hid_indicators_get_current_profile();
#else
    return 0;
#endif
}

static void hid_state_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t modifiers = (uint8_t)zmk_hid_get_explicit_mods();
    uint8_t indicators = hid_state_indicators();
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count && pipe < ESB_LINK_PIPE_MAX; pipe++) {
        if (hop_pipe_needs_rendezvous(pipe)) {
            /* Rejoin beacon carries HID state the cache never saw. */
            sent_known[pipe] = false;
            continue;
        }
        if (sent_known[pipe] && sent_modifiers[pipe] == modifiers
            && sent_indicators[pipe] == indicators) {
            continue;
        }
        int error = hop_stage_beacon(pipe, modifiers, indicators);
        if (error) {
            LOG_DBG("HID state to pipe %u not staged (%d)", pipe, error);
            continue;
        }
        sent_known[pipe] = true;
        sent_modifiers[pipe] = modifiers;
        sent_indicators[pipe] = indicators;
    }
}

static K_WORK_DEFINE(hid_state_work, hid_state_work_fn);

/* Listener order against ZMK's HID state update undefined. */
static int hid_state_listener(const zmk_event_t *eh) {
    if (as_zmk_keycode_state_changed(eh) != NULL) {
        k_work_submit(&hid_state_work);
    }
    return 0;
}

ZMK_LISTENER(esb_hid_state, hid_state_listener);
ZMK_SUBSCRIPTION(esb_hid_state, zmk_keycode_state_changed);

#if defined(CONFIG_ZMK_HID_INDICATORS)
static int hid_state_indicators_listener(const zmk_event_t *eh) {
    if (as_zmk_hid_indicators_changed(eh) != NULL) {
        k_work_submit(&hid_state_work);
    }
    return 0;
}

ZMK_LISTENER(esb_hid_state_indicators, hid_state_indicators_listener);
ZMK_SUBSCRIPTION(esb_hid_state_indicators, zmk_hid_indicators_changed);
#endif

uint8_t zmk_split_esb_hid_modifiers(void) {
    return (uint8_t)zmk_hid_get_explicit_mods();
}

uint8_t zmk_split_esb_hid_indicators(void) {
    return hid_state_indicators();
}
