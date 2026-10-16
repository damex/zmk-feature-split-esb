// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap feeding a full duplex wire peer's UART.
 * Exits 0 once two beacon frames one peer longer than its own log one refusal warning with
 * both lengths and no dropped command.
 * Exits 1 on no warning, a repeated one, one without both lengths, or a dropped command.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "hop_internal.h"
#include "mock.h"
#include "mock_log.h"
#include "mock_wire.h"

#define CENTRAL_BEACON_LENGTH (sizeof(struct esb_beacon) + sizeof(struct esb_beacon_peer))
#define BEACON_FRAMES 2
#define REFUSAL_PREFIX "Refusing beacon length"
#define COMMAND_DROP "Dropping command"
#define REFUSAL_LENGTH_MAX 96
#define INJECT_DELAY_MS 50
#define INJECT_GAP_MS 20
#define VERDICT_MS 150

static const uint8_t central_beacon[CENTRAL_BEACON_LENGTH] = {
    [offsetof(struct esb_beacon, tag)] = ESB_BEACON_TAG,
};
static size_t frames_injected;

static void inject_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(inject_work, inject_fn);

static void inject_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_wire_rx_inject(central_beacon, sizeof(central_beacon));
    frames_injected++;
    if (frames_injected < BEACON_FRAMES) {
        k_work_reschedule(&inject_work, K_MSEC(INJECT_GAP_MS));
    }
}

static void check_refusal_lengths(void) {
    char refusal[REFUSAL_LENGTH_MAX];
    int written = snprintf(refusal,
                           sizeof(refusal),
                           REFUSAL_PREFIX " %u from central, expected %u for this peripheral list",
                           (unsigned int)CENTRAL_BEACON_LENGTH,
                           (unsigned int)ESB_BEACON_LENGTH);
    bool refusal_fits = written > 0 && (size_t)written < sizeof(refusal);
    mock_check(refusal_fits, "refusal text fits its buffer");
    mock_check(mock_log_count(refusal) == 1, "refusal names both beacon lengths");
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(frames_injected == BEACON_FRAMES, "both longer beacon frames injected");
    mock_check(mock_log_count(REFUSAL_PREFIX) == 1, "two longer beacon frames log one refusal");
    mock_check(mock_log_count(COMMAND_DROP) == 0, "longer beacon frames drop no command");
    check_refusal_lengths();
    printk("PASS: all %u wire peer warning checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_wire_init(void) {
    k_work_reschedule(&inject_work, K_MSEC(INJECT_DELAY_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_wire_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
