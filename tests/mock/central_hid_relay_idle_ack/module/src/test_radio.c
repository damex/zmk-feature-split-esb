// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central with a relay dongle and ZMK's mouse key press.
 * Keymap holds LCLK, then nothing changes while the dongle polls faster than the refresh.
 * Exits 0 once idle polls wrote only full refreshes holding LCLK.
 * Exits 1 on any other idle relay ACK, more refreshes than the period allows, or none.
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

#include <dt-bindings/zmk/pointing.h>
#include <zmk/hid.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

#define RELAY_PIPE DT_PROP(DT_NODELABEL(relay), pipe)
#define REPORT_ID_OFFSET 0
#define RELAY_POLL_MS 1
#define POLL_START_MS 20
#define IDLE_START_MS 100
#define VERDICT_MS 400
#define CONSUMER_OFFSET sizeof(struct zmk_hid_keyboard_report)
#define POINTER_OFFSET (CONSUMER_OFFSET + sizeof(struct zmk_hid_consumer_report))
#define REFRESH_LENGTH (POINTER_OFFSET + sizeof(struct zmk_hid_mouse_report))
#define REFRESHES_MAX ((VERDICT_MS - IDLE_START_MS) / CONFIG_ZMK_SPLIT_ESB_HID_RELAY_KEEPALIVE_MS + 1)

BUILD_ASSERT(RELAY_POLL_MS < CONFIG_ZMK_SPLIT_ESB_HID_RELAY_KEEPALIVE_MS,
             "dongle must poll faster than the refresh");

static size_t idle_polls;
static size_t idle_refreshes;

static bool is_refresh_holding_lclk(const struct esb_payload *payload) {
    if (payload->length != REFRESH_LENGTH) {
        return false;
    }
    if (payload->data[REPORT_ID_OFFSET] != ZMK_HID_REPORT_ID_KEYBOARD) {
        return false;
    }
    if (payload->data[CONSUMER_OFFSET + REPORT_ID_OFFSET] != ZMK_HID_REPORT_ID_CONSUMER) {
        return false;
    }
    const struct zmk_hid_mouse_report held = {
        .report_id = ZMK_HID_REPORT_ID_MOUSE,
        .body = {.buttons = LCLK},
    };
    return memcmp(&payload->data[POINTER_OFFSET], &held, sizeof(held)) == 0;
}

int esb_write_payload(const struct esb_payload *payload) {
    if (k_uptime_get() < IDLE_START_MS) {
        return 0;
    }
    if (!is_refresh_holding_lclk(payload)) {
        printk("FAIL: idle relay ACK of %u bytes is not a full refresh holding LCLK\n",
               (unsigned int)payload->length);
        exit(1);
    }
    idle_refreshes++;
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    ARG_UNUSED(callback);
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

static void relay_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_fn);

static void relay_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (k_uptime_get() >= IDLE_START_MS) {
        idle_polls++;
    }
    esb_link_role_rx_done((uint8_t)BIT(RELAY_PIPE));
    k_work_reschedule(&relay_poll_work, K_MSEC(RELAY_POLL_MS));
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (idle_refreshes == 0) {
        printk("FAIL: no refresh reached the dongle in %u idle polls\n", (unsigned int)idle_polls);
        exit(1);
    }
    if (idle_refreshes > REFRESHES_MAX) {
        printk("FAIL: %u idle relay ACKs, the refresh period allows %u\n",
               (unsigned int)idle_refreshes, (unsigned int)REFRESHES_MAX);
        exit(1);
    }
    printk("PASS: %u idle polls wrote only %u full refreshes holding LCLK\n",
           (unsigned int)idle_polls, (unsigned int)idle_refreshes);
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_START_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
