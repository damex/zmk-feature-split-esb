// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay, central half: keyboard and consumer reports out to relay-role pipes. */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

#include "esb_link.h"
#include "esb_link_internal.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static void stage_report(uint8_t pipe, const void *report, size_t length) {
    int error = esb_link_stage_reply(pipe, report, length);
    if (error != 0) {
        LOG_WRN("hid relay stage failed pipe %u err %d", pipe, error);
    }
}

static void stage_keyboard_report(uint8_t pipe) {
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    stage_report(pipe, report, sizeof(*report));
}

static void stage_consumer_report(uint8_t pipe) {
    struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
    stage_report(pipe, report, sizeof(*report));
}

BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report) + sizeof(struct zmk_hid_consumer_report) <=
                 CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "keyboard and consumer reports must fit one relay reply");

static void latch_current_reports(uint8_t pipe) {
    struct zmk_hid_keyboard_report *keyboard = zmk_hid_get_keyboard_report();
    struct zmk_hid_consumer_report *consumer = zmk_hid_get_consumer_report();
    uint8_t reply[sizeof(*keyboard) + sizeof(*consumer)];
    memcpy(reply, keyboard, sizeof(*keyboard));
    memcpy(&reply[sizeof(*keyboard)], consumer, sizeof(*consumer));
    int error = esb_link_latch_idle_reply(pipe, reply, sizeof(reply));
    if (error != 0) {
        LOG_WRN("hid relay latch failed pipe %u err %d", pipe, error);
    }
}

static void hid_relay_keepalive_fire(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hid_relay_keepalive_work, hid_relay_keepalive_fire);

static void hid_relay_keepalive_fire(struct k_work *work) {
    ARG_UNUSED(work);
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        if (esb_link_pipe_is_relay(pipe)) {
            latch_current_reports(pipe);
        }
    }
    k_work_reschedule(&hid_relay_keepalive_work,
                      K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_KEEPALIVE_MS));
}

/* Every change queues in order, a tap's press and release both reach the host. */
static int hid_relay_listener(const zmk_event_t *event) {
    const struct zmk_keycode_state_changed *keycode = as_zmk_keycode_state_changed(event);
    if (keycode == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        if (!esb_link_pipe_is_relay(pipe)) {
            continue;
        }
        if (keycode->usage_page == HID_USAGE_CONSUMER) {
            stage_consumer_report(pipe);
        } else {
            stage_keyboard_report(pipe);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(esb_hid_relay, hid_relay_listener);
ZMK_SUBSCRIPTION(esb_hid_relay, zmk_keycode_state_changed);

static int hid_relay_keepalive_init(void) {
    k_work_reschedule(&hid_relay_keepalive_work,
                      K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_KEEPALIVE_MS));
    return 0;
}
SYS_INIT(hid_relay_keepalive_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
