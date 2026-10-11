// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a peripheral whose retry load rises and falls.
 * Exits 0 once the retransmit count holds the floor on a clean link, climbs to the ceiling
 * under retries, obeys a lowered runtime ceiling and falls back to the floor.
 * Exits 1 on a count moving the wrong way, a missed bound, or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "hop.h"
#include "mock.h"
#include "mock_esb.h"

#define RETRANSMIT_FLOOR 2
#define RETRANSMIT_CEILING DT_INST_PROP(0, retransmit_count)
#define LOWERED_CEILING 6
#define FIRST_TRY_ATTEMPTS 1
#define LAST_TRY_ATTEMPTS (RETRANSMIT_CEILING + 1)
#define CLEAN_WINDOWS 4
#define VERDICT_DEADLINE_MS 2000

BUILD_ASSERT(LOWERED_CEILING > RETRANSMIT_FLOOR, "lowered ceiling stays above the floor");
BUILD_ASSERT(LOWERED_CEILING < RETRANSMIT_CEILING, "lowered ceiling sits below the DT ceiling");

enum phase {
    PHASE_CLEAN,
    PHASE_CLIMB,
    PHASE_LOWERED,
    PHASE_RECOVERY,
};

static const uint32_t phase_attempts[] = {
    [PHASE_CLEAN] = FIRST_TRY_ATTEMPTS,
    [PHASE_CLIMB] = LAST_TRY_ATTEMPTS,
    [PHASE_LOWERED] = LAST_TRY_ATTEMPTS,
    [PHASE_RECOVERY] = FIRST_TRY_ATTEMPTS,
};

static enum phase phase = PHASE_CLEAN;
static size_t clean_seen;
static uint16_t last_count;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(phase_attempts[phase]);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static bool link_searching(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    return status.searching;
}

static void check_clean(uint16_t count) {
    mock_check(count == RETRANSMIT_FLOOR, "clean link keeps the retransmit floor");
    clean_seen++;
    if (clean_seen == CLEAN_WINDOWS) {
        phase = PHASE_CLIMB;
    }
}

static void check_climb(uint16_t count) {
    mock_check(count >= last_count, "retries never lower the retransmit count");
    mock_check(count <= RETRANSMIT_CEILING, "retransmit count stays within the DT ceiling");
    if (count == RETRANSMIT_CEILING) {
        mock_check(hop_set_retransmit_ceiling(LOWERED_CEILING) == 0,
                   "runtime ceiling below the DT one accepted");
        phase = PHASE_LOWERED;
    }
}

static void check_lowered(uint16_t count) {
    mock_check(count == LOWERED_CEILING, "lowered runtime ceiling caps the retransmit count");
    phase = PHASE_RECOVERY;
}

static void check_recovery(uint16_t count) {
    mock_check(count <= last_count, "first-try transmits never raise the retransmit count");
    if (count == RETRANSMIT_FLOOR) {
        printk("PASS: all %u adaptive retransmit checks\n", (unsigned int)mock_checks_passed());
        exit(0);
    }
}

static void check_window(uint16_t count) {
    switch (phase) {
    case PHASE_CLEAN:
        check_clean(count);
        break;
    case PHASE_CLIMB:
        check_climb(count);
        break;
    case PHASE_LOWERED:
        check_lowered(count);
        break;
    case PHASE_RECOVERY:
        check_recovery(count);
        break;
    default:
        __ASSERT_NO_MSG(false);
        break;
    }
    last_count = count;
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length) && !link_searching()) {
        check_window(mock_esb_retransmit_count());
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: retransmit count %u stuck in phase %u\n", (unsigned int)last_count,
           (unsigned int)phase);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
