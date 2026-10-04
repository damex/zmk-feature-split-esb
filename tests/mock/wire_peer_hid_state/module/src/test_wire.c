// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap feeding a wire peer's UART.
 * Exits 0 once a beacon frame lands as the wire peer's HID state.
 * Exits 1 at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/modifiers.h>

#include <zmk_split_esb.h>

#include "hop_internal.h"
#include "wire_frame.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define WIRE_UART DEVICE_DT_GET(DT_CHOSEN(zmk_esb_wire))
#define INJECT_DELAY_MS 50
#define STATE_POLL_MS 4
#define VERDICT_DEADLINE_MS 500
#define MODIFIERS MOD_LSFT
#define INDICATORS 0x02

static void inject_fn(struct k_work *work) {
    ARG_UNUSED(work);
    struct esb_beacon beacon = {
        .tag = ESB_BEACON_TAG,
        .hid_modifiers = MODIFIERS,
        .hid_indicators = INDICATORS,
    };
    uint8_t frame[WIRE_FRAME_MAX_ENCODED];
    int frame_length =
        wire_frame_encode((const uint8_t *)&beacon, sizeof(beacon), frame, sizeof(frame));
    if (frame_length < 0) {
        printk("FAIL: beacon frame encode returned %d\n", frame_length);
        exit(1);
    }
    uint32_t accepted = uart_emul_put_rx_data(WIRE_UART, frame, (size_t)frame_length);
    if (accepted != (uint32_t)frame_length) {
        printk("FAIL: wire rx took %u of %d bytes\n", (unsigned int)accepted, frame_length);
        exit(1);
    }
}
static K_WORK_DELAYABLE_DEFINE(inject_work, inject_fn);

static void state_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(state_poll_work, state_poll_fn);

static void state_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (zmk_split_esb_hid_modifiers() == MODIFIERS && zmk_split_esb_hid_indicators() == INDICATORS) {
        printk("PASS: wire peer took modifiers 0x%02x, indicators 0x%02x from a beacon frame\n",
               MODIFIERS, INDICATORS);
        exit(0);
    }
    k_work_reschedule(&state_poll_work, K_MSEC(STATE_POLL_MS));
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: wire peer holds modifiers 0x%02x, indicators 0x%02x, beacon carried 0x%02x, "
           "0x%02x\n",
           zmk_split_esb_hid_modifiers(), zmk_split_esb_hid_indicators(), MODIFIERS, INDICATORS);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_wire_init(void) {
    k_work_reschedule(&inject_work, K_MSEC(INJECT_DELAY_MS));
    k_work_reschedule(&state_poll_work, K_MSEC(STATE_POLL_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_wire_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
