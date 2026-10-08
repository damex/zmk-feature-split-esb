// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central with a relay dongle, keys and a pointer.
 * Central gets its own USB host while a key and a button are held, then loses it.
 * Exits 0 once the relay forwards before and after,
 * and while paused sends only released keys, one button release and no motion,
 * ignoring the dongle's host LEDs.
 * Exits 1 on a held key or motion while paused, applied LEDs, or a missing phase at verdict.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_indicators.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid.h>

#include <esb.h>

#include "esb_hid_state.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define REPORT_ID_OFFSET 0
#define REPORT_BODY_OFFSET 1
#define PRIMARY_BUTTON BIT(0)
#define ACTIVE_POINTER_X 4
#define POLL_START_MS 20
#define PAUSE_MS 100
#define LEDS_MS 250
#define RESUME_MS 300
#define VERDICT_MS 500

BUILD_ASSERT(offsetof(struct zmk_hid_keyboard_report, body) == REPORT_BODY_OFFSET,
             "keyboard report body follows its id");

enum phase {
    PHASE_ACTIVE,
    PHASE_PAUSED,
    PHASE_RESUMED,
};

struct observations {
    bool active_key_a;
    bool active_button;
    int32_t active_pointer_x;
    bool paused_release;
    bool paused_button_release;
    bool resumed_key_c;
};

static enum phase phase = PHASE_ACTIVE;
static struct observations seen;
static esb_link_rx_callback_t rx_callback;

static bool body_released(const uint8_t *report, size_t length) {
    for (size_t offset = REPORT_BODY_OFFSET; offset < length; offset++) {
        if (report[offset] != 0) {
            return false;
        }
    }
    return true;
}

static bool keyboard_holds(const struct zmk_hid_keyboard_report *report, uint32_t usage) {
    for (size_t index = 0; index < ARRAY_SIZE(report->body.keys); index++) {
        if (report->body.keys[index] == ZMK_HID_USAGE_ID(usage)) {
            return true;
        }
    }
    return false;
}

static void observe_keyboard(const uint8_t *bytes) {
    struct zmk_hid_keyboard_report report = {0};
    memcpy(&report, bytes, sizeof(report));
    switch (phase) {
    case PHASE_ACTIVE:
        seen.active_key_a |= keyboard_holds(&report, A);
        break;
    case PHASE_PAUSED:
        if (!body_released(bytes, sizeof(report))) {
            printk("FAIL: paused relay sent a held key\n");
            exit(1);
        }
        seen.paused_release = true;
        break;
    case PHASE_RESUMED:
        seen.resumed_key_c |= keyboard_holds(&report, C);
        break;
    default:
        printk("FAIL: unknown phase %d\n", (int)phase);
        exit(1);
    }
}

static void observe_consumer(const uint8_t *bytes) {
    if (phase == PHASE_PAUSED && !body_released(bytes, sizeof(struct zmk_hid_consumer_report))) {
        printk("FAIL: paused relay sent a held consumer key\n");
        exit(1);
    }
}

static bool pointer_moves(const struct zmk_hid_mouse_report *report) {
    const struct zmk_hid_mouse_report_body *body = &report->body;
    return body->d_x != 0 || body->d_y != 0 || body->d_scroll_y != 0 || body->d_scroll_x != 0;
}

static void observe_pointer(const uint8_t *bytes) {
    struct zmk_hid_mouse_report report = {0};
    memcpy(&report, bytes, sizeof(report));
    if (phase == PHASE_ACTIVE) {
        seen.active_pointer_x += report.body.d_x;
        seen.active_button |= (report.body.buttons & PRIMARY_BUTTON) != 0;
        return;
    }
    if (phase != PHASE_PAUSED) {
        return;
    }
    if (pointer_moves(&report)) {
        printk("FAIL: paused relay sent pointer motion\n");
        exit(1);
    }
    seen.paused_button_release |= report.body.buttons == 0;
}

static size_t observe_report(const uint8_t *bytes, size_t room) {
    uint8_t report_id = bytes[REPORT_ID_OFFSET];
    if (report_id == ZMK_HID_REPORT_ID_KEYBOARD && room >= sizeof(struct zmk_hid_keyboard_report)) {
        observe_keyboard(bytes);
        return sizeof(struct zmk_hid_keyboard_report);
    }
    if (report_id == ZMK_HID_REPORT_ID_CONSUMER && room >= sizeof(struct zmk_hid_consumer_report)) {
        observe_consumer(bytes);
        return sizeof(struct zmk_hid_consumer_report);
    }
    if (report_id == ZMK_HID_REPORT_ID_MOUSE && room >= sizeof(struct zmk_hid_mouse_report)) {
        observe_pointer(bytes);
        return sizeof(struct zmk_hid_mouse_report);
    }
    printk("FAIL: relay ACK holds a report that is not whole, id 0x%02x\n", report_id);
    exit(1);
}

int esb_write_payload(const struct esb_payload *payload) {
    size_t offset = 0;
    while (offset < payload->length) {
        offset += observe_report(&payload->data[offset], payload->length - offset);
    }
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    rx_callback = callback;
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    return 0;
}

int esb_link_hfclk_acquire(void) {
    return 0;
}

void hop_boot_mask(void) {
}

uint32_t hop_pipe_quiet_ms(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return 0;
}

bool hop_pipe_heard(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return false;
}

static int indicators_listener(const zmk_event_t *event) {
    const struct zmk_hid_indicators_changed *changed = as_zmk_hid_indicators_changed(event);
    if (changed != NULL && changed->indicators == HID_INDICATOR_CAPS_LOCK) {
        printk("FAIL: dongle host LEDs applied while the central has its own host\n");
        exit(1);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(central_hid_relay_host_pause_test, indicators_listener);
ZMK_SUBSCRIPTION(central_hid_relay_host_pause_test, zmk_hid_indicators_changed);

static void raise_host(enum zmk_transport transport) {
    raise_zmk_endpoint_changed(
        (struct zmk_endpoint_changed){.endpoint = {.transport = transport}});
}

static void pause_fn(struct k_work *work) {
    ARG_UNUSED(work);
    phase = PHASE_PAUSED;
    raise_host(ZMK_TRANSPORT_USB);
}
static K_WORK_DELAYABLE_DEFINE(pause_work, pause_fn);

static void leds_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct esb_host_indicators packet = {
        .tag = ESB_HOST_INDICATORS_TAG,
        .indicators = HID_INDICATOR_CAPS_LOCK,
    };
    rx_callback(RELAY_PIPE, (const uint8_t *)&packet, sizeof(packet));
}
static K_WORK_DELAYABLE_DEFINE(leds_work, leds_fn);

static void resume_fn(struct k_work *work) {
    ARG_UNUSED(work);
    phase = PHASE_RESUMED;
    raise_host(ZMK_TRANSPORT_NONE);
}
static K_WORK_DELAYABLE_DEFINE(resume_work, resume_fn);

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    esb_link_role_rx_done((uint8_t)BIT(RELAY_PIPE));
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
}

static void check_seen(bool observed, const char *what) {
    if (!observed) {
        printk("FAIL: never saw %s\n", what);
        exit(1);
    }
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    check_seen(seen.active_key_a, "key A relayed before the pause");
    check_seen(seen.active_button, "button relayed before the pause");
    check_seen(seen.active_pointer_x == ACTIVE_POINTER_X, "pointer motion relayed before the pause");
    check_seen(seen.paused_release, "released keys sent while paused");
    check_seen(seen.paused_button_release, "button release sent while paused");
    check_seen(seen.resumed_key_c, "key C relayed after the pause");
    printk("PASS: relay paused for the central's own host and resumed after it\n");
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_START_MS));
    k_work_reschedule(&pause_work, K_MSEC(PAUSE_MS));
    k_work_reschedule(&leds_work, K_MSEC(LEDS_MS));
    k_work_reschedule(&resume_work, K_MSEC(RESUME_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
