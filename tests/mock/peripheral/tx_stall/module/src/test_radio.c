// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral whose TX FIFO stops taking writes.
 * Exits 0 once a full FIFO right after an ack is refused without a flush, and a FIFO jammed
 * with no TX events is flushed once after several refusals, the retry landing.
 * Exits 1 on an early or extra flush, a missing retry, or at deadline.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "mock.h"
#include "mock_esb.h"

#define WARMUP_WRITES 5
#define FIRST_TRY_ATTEMPTS 1
#define VERDICT_DEADLINE_MS 2000

enum phase {
    PHASE_WARMUP,
    PHASE_BUSY_AFTER_ACK,
    PHASE_JAMMED,
};

static enum phase phase = PHASE_WARMUP;
static size_t writes;
static size_t refused_writes;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static int warmup_write(void) {
    if (writes < WARMUP_WRITES) {
        k_work_submit(&tx_success_work);
        return 0;
    }
    phase = PHASE_BUSY_AFTER_ACK;
    refused_writes++;
    return -ENOMEM;
}

static int busy_after_ack_write(void) {
    mock_check(refused_writes == 1, "full FIFO right after an ack refused once, no retry");
    mock_check(mock_esb_flush_count() == 0, "full FIFO right after an ack left unflushed");
    phase = PHASE_JAMMED;
    refused_writes = 1;
    return -ENOMEM;
}

static int jammed_write(void) {
    if (mock_esb_flush_count() == 0) {
        refused_writes++;
        return -ENOMEM;
    }
    mock_check(mock_esb_flush_count() == 1, "jammed FIFO flushed once");
    mock_check(refused_writes >= 2, "jammed FIFO refused more than once before the flush");
    printk("PASS: all %u tx stall checks, %u refusals before the flush\n",
           (unsigned int)mock_checks_passed(), (unsigned int)refused_writes);
    exit(0);
}

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    writes++;
    switch (phase) {
    case PHASE_WARMUP:
        return warmup_write();
    case PHASE_BUSY_AFTER_ACK:
        return busy_after_ack_write();
    case PHASE_JAMMED:
        return jammed_write();
    default:
        printk("FAIL: unknown phase %d\n", (int)phase);
        exit(1);
    }
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: phase %d after %u writes, %u refused, %u flushes by deadline\n", (int)phase,
           (unsigned int)writes, (unsigned int)refused_writes,
           (unsigned int)mock_esb_flush_count());
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
