// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay, central half: keyboard and consumer reports out to relay-role pipes. */

#include "esb_hid_relay_central.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

#include "esb_hid_relay_pointer.h"
#include "esb_link.h"
#include "esb_link_internal.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static atomic_t paused;

static const struct zmk_hid_keyboard_report released_keyboard = {
    .report_id = ZMK_HID_REPORT_ID_KEYBOARD,
};
static const struct zmk_hid_consumer_report released_consumer = {
    .report_id = ZMK_HID_REPORT_ID_CONSUMER,
};

bool esb_hid_relay_active(void) {
    return atomic_get(&paused) == 0;
}

static void stage_report(uint8_t pipe, const void *report, size_t length) {
    int error = esb_link_stage_reply(pipe, report, length);
    if (error != 0) {
        LOG_WRN("hid relay stage failed pipe %u err %d", pipe, error);
    }
}

void esb_hid_relay_stage(const void *report, size_t length) {
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        if (!esb_link_pipe_is_relay(pipe)) {
            continue;
        }
        stage_report(pipe, report, length);
    }
}

BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report) + sizeof(struct zmk_hid_consumer_report) <=
                 CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "keyboard and consumer reports must fit one relay reply");

static void latch_reports(uint8_t pipe, const struct zmk_hid_keyboard_report *keyboard,
                          const struct zmk_hid_consumer_report *consumer) {
    uint8_t reply[sizeof(*keyboard) + sizeof(*consumer)];
    memcpy(reply, keyboard, sizeof(*keyboard));
    memcpy(&reply[sizeof(*keyboard)], consumer, sizeof(*consumer));
    int error = esb_link_latch_idle_reply(pipe, reply, sizeof(reply));
    if (error != 0) {
        LOG_WRN("hid relay latch failed pipe %u err %d", pipe, error);
    }
}

static void latch_relay_reports(void) {
    bool active = esb_hid_relay_active();
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        if (!esb_link_pipe_is_relay(pipe)) {
            continue;
        }
        if (active) {
            latch_reports(pipe, zmk_hid_get_keyboard_report(), zmk_hid_get_consumer_report());
        } else {
            latch_reports(pipe, &released_keyboard, &released_consumer);
        }
    }
}

static void hid_relay_keepalive_fire(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hid_relay_keepalive_work, hid_relay_keepalive_fire);

static void hid_relay_keepalive_fire(struct k_work *work) {
    ARG_UNUSED(work);
    latch_relay_reports();
    k_work_reschedule(&hid_relay_keepalive_work,
                      K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_KEEPALIVE_MS));
}

/* Every change queues in order, a tap's press and release both reach the host. */
static void stage_keycode(const struct zmk_keycode_state_changed *keycode) {
    if (!esb_hid_relay_active()) {
        return;
    }
    if (keycode->usage_page == HID_USAGE_CONSUMER) {
        struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
        esb_hid_relay_stage(report, sizeof(*report));
    } else {
        struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
        esb_hid_relay_stage(report, sizeof(*report));
    }
}

/* Swap the refresh now, an older one must not reach the dongle after the change. */
static void follow_endpoint(const struct zmk_endpoint_changed *changed) {
    atomic_set(&paused, changed->endpoint.transport != ZMK_TRANSPORT_NONE);
    esb_hid_relay_pointer_reset();
    latch_relay_reports();
}

static int hid_relay_listener(const zmk_event_t *event) {
    const struct zmk_keycode_state_changed *keycode = as_zmk_keycode_state_changed(event);
    if (keycode != NULL) {
        stage_keycode(keycode);
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_endpoint_changed *changed = as_zmk_endpoint_changed(event);
    if (changed != NULL) {
        follow_endpoint(changed);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(esb_hid_relay, hid_relay_listener);
ZMK_SUBSCRIPTION(esb_hid_relay, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(esb_hid_relay, zmk_endpoint_changed);

static int hid_relay_keepalive_init(void) {
    k_work_reschedule(&hid_relay_keepalive_work,
                      K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_KEEPALIVE_MS));
    return 0;
}
SYS_INIT(hid_relay_keepalive_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
