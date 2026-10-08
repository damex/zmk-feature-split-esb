// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a pointer peripheral fed by an input mock.
 * TX FIFO refuses every write for FIFO_FULL_MS from each button event on, or until flushed.
 * Exits 0 once every input event was offered to the radio once and in order, lossy-codes motion
 * without ACK, and the first keepalive after each full FIFO lists the refused button state.
 * Exits 1 on a wrong packet, a resent event, a wrong heal, no heal within HEAL_BOUND_MS
 * of the FIFO draining, or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/types.h>

#include <esb.h>

#include "esb_batch.h"
#include "esb_keepalive.h"
#include "esb_wire.h"
#include "mock.h"
#include "mock_esb.h"

#define FIRST_TRY_ATTEMPTS 1
#define FIFO_FULL_MS 50
#define HEAL_BOUND_MS (DT_INST_PROP(0, idle_keepalive_ms) + DT_INST_PROP(0, hop_window_ms))
#define VERDICT_DEADLINE_MS 1500
#define POINTER_REG DT_REG_ADDR(DT_NODELABEL(split_pointer))

enum mock_event_cell {
    MOCK_EVENT_TYPE,
    MOCK_EVENT_CODE,
    MOCK_EVENT_VALUE,
    MOCK_EVENT_SYNC,
    MOCK_EVENT_CELLS,
};

static const uint32_t mock_events[] = DT_PROP(DT_NODELABEL(pointer_input), events);
BUILD_ASSERT(ARRAY_SIZE(mock_events) % MOCK_EVENT_CELLS == 0, "input-mock events are tuples of four cells");
#define MOCK_EVENT_COUNT (ARRAY_SIZE(mock_events) / MOCK_EVENT_CELLS)

static const uint32_t lossy_codes[] = DT_INST_PROP(0, lossy_codes);

struct packet_summary {
    bool holds_button;
    bool button_pressed;
    bool lossy_only;
    bool full_batch;
};

static size_t next_event;
static bool fifo_fill_started;
static uint32_t fifo_full_until_ms;
static size_t flushes_at_fill;
static bool heal_pending;
static bool refused_button_pressed;
static bool press_healed;
static bool release_healed;
static size_t refused_motion_packets;
static size_t full_batches;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static void heal_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: no keepalive healed the refused button within %u ms of the FIFO draining\n",
           (unsigned int)HEAL_BOUND_MS);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(heal_deadline_work, heal_deadline_fn);

static bool event_is_lossy(const struct zmk_split_transport_peripheral_event *event) {
    for (size_t pair = 0; pair < ARRAY_SIZE(lossy_codes); pair += 2) {
        if (event->data.input_event.type == lossy_codes[pair] &&
            event->data.input_event.code == lossy_codes[pair + 1]) {
            return true;
        }
    }
    return false;
}

static bool event_is_button(const struct zmk_split_transport_peripheral_event *event) {
    return event->data.input_event.type == INPUT_EV_KEY;
}

static bool event_is_mock_event_at(const struct zmk_split_transport_peripheral_event *event,
                                   size_t index) {
    const uint32_t *cells = &mock_events[index * MOCK_EVENT_CELLS];
    return event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT &&
           event->data.input_event.reg == POINTER_REG &&
           event->data.input_event.type == cells[MOCK_EVENT_TYPE] &&
           event->data.input_event.code == cells[MOCK_EVENT_CODE] &&
           event->data.input_event.value == (int32_t)cells[MOCK_EVENT_VALUE] &&
           (event->data.input_event.sync != 0) == (cells[MOCK_EVENT_SYNC] != 0);
}

static bool event_ends_batch(const struct zmk_split_transport_peripheral_event *event,
                             size_t batched) {
    return event->data.input_event.sync != 0 || event->data.input_event.type == INPUT_EV_KEY ||
           batched == ESB_BATCH_MAX;
}

static struct packet_summary check_event_packet(const struct esb_payload *payload) {
    struct packet_summary summary = {.lossy_only = true};
    size_t offset = 0;
    size_t batched = 0;
    bool batch_ended = false;
    bool last_sync = false;
    while (offset < payload->length) {
        struct zmk_split_transport_peripheral_event event = {0};
        size_t consumed =
            esb_wire_decode_event(&payload->data[offset], payload->length - offset, &event);
        mock_check(consumed != 0, "event packet decodes whole");
        mock_check(!batch_ended, "packet ends at a sync, a button or a full batch");
        mock_check(next_event < MOCK_EVENT_COUNT && event_is_mock_event_at(&event, next_event),
                   "every input event is offered to the radio once and in order");
        next_event++;
        batched++;
        batch_ended = event_ends_batch(&event, batched);
        last_sync = event.data.input_event.sync != 0;
        if (event_is_button(&event)) {
            summary.holds_button = true;
            summary.button_pressed = event.data.input_event.value != 0;
        }
        if (!event_is_lossy(&event)) {
            summary.lossy_only = false;
        }
        offset += consumed;
    }
    mock_check(batch_ended, "packet ends at a sync, a button or a full batch");
    summary.full_batch = batched == ESB_BATCH_MAX && !last_sync && !summary.holds_button;
    return summary;
}

static bool fifo_full(void) {
    return fifo_fill_started && mock_esb_flush_count() == flushes_at_fill &&
           (int32_t)(k_uptime_get_32() - fifo_full_until_ms) < 0;
}

static void fill_fifo(bool button_pressed) {
    mock_check(!heal_pending, "a keepalive healed the last refused button before the next one");
    fifo_fill_started = true;
    fifo_full_until_ms = k_uptime_get_32() + FIFO_FULL_MS;
    flushes_at_fill = mock_esb_flush_count();
    refused_button_pressed = button_pressed;
    heal_pending = true;
    k_work_reschedule(&heal_deadline_work, K_MSEC(FIFO_FULL_MS + HEAL_BOUND_MS));
}

static void check_press_heal(const uint8_t *keepalive) {
    mock_check(esb_keepalive_held_count(keepalive) == 1,
               "first keepalive after the refused press holds one input key");
    struct esb_keepalive_held_key key = esb_keepalive_held_key_at(keepalive, 0);
    mock_check(key.reg == POINTER_REG && key.code == INPUT_BTN_0,
               "first keepalive after the refused press lists it as held");
    press_healed = true;
}

static void check_release_heal(const uint8_t *keepalive) {
    mock_check(esb_keepalive_held_count(keepalive) == 0,
               "first keepalive after the refused release holds no input key");
    release_healed = true;
}

static void check_heal(const uint8_t *keepalive) {
    if (refused_button_pressed) {
        check_press_heal(keepalive);
    } else {
        check_release_heal(keepalive);
    }
    heal_pending = false;
    k_work_cancel_delayable(&heal_deadline_work);
}

static void check_verdict(void) {
    if (next_event < MOCK_EVENT_COUNT || heal_pending) {
        return;
    }
    mock_check(refused_motion_packets != 0, "full FIFO refused lossy motion");
    mock_check(press_healed && release_healed,
               "keepalives healed a refused press and a refused release");
    mock_check(full_batches != 0, "a full batch is offered without a sync");
    printk("PASS: %u input events offered to the radio in order, %u motion packets refused, "
           "keepalives healed the refused press and release\n",
           (unsigned int)MOCK_EVENT_COUNT, (unsigned int)refused_motion_packets);
    exit(0);
}

static int write_keepalive(const struct esb_payload *payload) {
    mock_check(!payload->noack, "keepalives ask for ACK");
    if (fifo_full()) {
        return -ENOMEM;
    }
    if (heal_pending) {
        check_heal(payload->data);
        check_verdict();
    }
    k_work_submit(&tx_success_work);
    return 0;
}

static int write_event_packet(const struct esb_payload *payload) {
    struct packet_summary summary = check_event_packet(payload);
    mock_check(payload->noack == summary.lossy_only,
               "lossy-codes motion goes without ACK, the rest with it");
    if (summary.full_batch) {
        full_batches++;
    }
    if (summary.holds_button) {
        fill_fifo(summary.button_pressed);
    }
    bool refused = fifo_full();
    if (refused && summary.lossy_only) {
        refused_motion_packets++;
    }
    if (!refused) {
        k_work_submit(&tx_success_work);
    }
    check_verdict();
    return refused ? -ENOMEM : 0;
}

int esb_write_payload(const struct esb_payload *payload) {
    if (esb_keepalive_matches(payload->data, payload->length)) {
        return write_keepalive(payload);
    }
    return write_event_packet(payload);
}

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: %u of %u input events offered to the radio by deadline\n",
           (unsigned int)next_event, (unsigned int)MOCK_EVENT_COUNT);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
