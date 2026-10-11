// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central with a relay dongle and ZMK's mouse key press.
 * Keymap holds LCLK across relay polls.
 * The ACK carrying its release never reaches the dongle.
 * Exits 0 once the next ACK releases the button on the dongle.
 * Exits 1 on an unknown report, no release ACK to lose,
 * or a release that misses the next ACK.
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
#define POLL_START_MS 20
#define VERDICT_MS 400

static uint8_t dongle_buttons;
static size_t ack_writes;
static size_t lost_ack_write;
static size_t heal_ack_write;

static size_t report_length(uint8_t report_id) {
    if (report_id == ZMK_HID_REPORT_ID_KEYBOARD) {
        return sizeof(struct zmk_hid_keyboard_report);
    }
    if (report_id == ZMK_HID_REPORT_ID_CONSUMER) {
        return sizeof(struct zmk_hid_consumer_report);
    }
    if (report_id == ZMK_HID_REPORT_ID_MOUSE) {
        return sizeof(struct zmk_hid_mouse_report);
    }
    return 0;
}

static uint8_t buttons_after(const struct esb_payload *payload) {
    uint8_t buttons = dongle_buttons;
    size_t offset = 0;
    while (offset < payload->length) {
        uint8_t report_id = payload->data[offset + REPORT_ID_OFFSET];
        size_t length = report_length(report_id);
        if (length == 0 || offset + length > payload->length) {
            printk("FAIL: relay ACK of %u bytes is not whole HID reports\n",
                   (unsigned int)payload->length);
            exit(1);
        }
        if (report_id == ZMK_HID_REPORT_ID_MOUSE) {
            struct zmk_hid_mouse_report report = {0};
            memcpy(&report, &payload->data[offset], sizeof(report));
            buttons = report.body.buttons;
        }
        offset += length;
    }
    return buttons;
}

int esb_write_payload(const struct esb_payload *payload) {
    ack_writes++;
    uint8_t buttons = buttons_after(payload);
    if (lost_ack_write == 0 && dongle_buttons == LCLK && buttons == 0) {
        lost_ack_write = ack_writes;
        return 0;
    }
    if (lost_ack_write != 0 && heal_ack_write == 0 && buttons == 0) {
        heal_ack_write = ack_writes;
    }
    dongle_buttons = buttons;
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
    esb_link_role_rx_done((uint8_t)BIT(RELAY_PIPE));
    k_work_reschedule(&relay_poll_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_HID_RELAY_POLL_MS));
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (lost_ack_write == 0) {
        printk("FAIL: no relay ACK carried the LCLK release to lose\n");
        exit(1);
    }
    if (dongle_buttons != 0) {
        printk("FAIL: dongle still holds buttons 0x%02x after losing the release ACK\n",
               dongle_buttons);
        exit(1);
    }
    if (heal_ack_write != lost_ack_write + 1) {
        printk("FAIL: release healed %u ACKs after the lost one, expected the next ACK\n",
               (unsigned int)(heal_ack_write - lost_ack_write));
        exit(1);
    }
    printk("PASS: next relay ACK released LCLK after the ACK carrying its release was lost\n");
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&relay_poll_work, K_MSEC(POLL_START_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
