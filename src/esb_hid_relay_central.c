// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay, central half: keyboard reports out to relay-role pipes. */

#include <errno.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

#include "esb_link.h"
#include "esb_link_internal.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static void stage_keyboard_report(uint8_t pipe) {
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    int error = esb_link_stage_reply(pipe, (const uint8_t *)report, sizeof(*report));
    if (error != 0) {
        LOG_WRN("hid relay stage failed pipe %u err %d", pipe, error);
    }
}

static void hid_relay_keepalive_fire(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hid_relay_keepalive_work, hid_relay_keepalive_fire);

/* Resend only on an idle queue, so a stale resend never jumps queued changes. */
static void hid_relay_keepalive_fire(struct k_work *work) {
    ARG_UNUSED(work);
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        if (esb_link_pipe_is_relay(pipe) && esb_link_reply_queue_empty(pipe)) {
            stage_keyboard_report(pipe);
        }
    }
    k_work_reschedule(&hid_relay_keepalive_work,
                      K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_KEEPALIVE_MS));
}

/* Every change queues in order, a tap's press and release both reach the host. */
static int hid_relay_listener(const zmk_event_t *event) {
    ARG_UNUSED(event);
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        if (esb_link_pipe_is_relay(pipe)) {
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
