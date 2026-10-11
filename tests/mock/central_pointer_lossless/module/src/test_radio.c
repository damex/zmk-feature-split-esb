// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central receiving a pointer peripheral's packets.
 * Exits 0 once the input split device replays every event with its code, value and sync,
 * in order, several to a packet.
 * Exits 1 on a wrong, missing or extra event.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/types.h>

#include <esb.h>

#include "central.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "esb_wire.h"
#include "hop.h"

#define PIPE DT_PROP(DT_NODELABEL(mouse), pipe)
#define POINTER_REG DT_REG_ADDR(DT_NODELABEL(split_pointer))
#define SEND_DELAY_MS 50
#define SETTLE_MS 100
#define VERDICT_DEADLINE_MS 1000
#define WIDE_MOTION 70000

struct pointer_event {
    uint8_t type;
    uint16_t code;
    int32_t value;
    bool sync;
    bool ends_packet;
};

static const struct pointer_event events[] = {
    {.type = INPUT_EV_REL, .code = INPUT_REL_X, .value = 5},
    {.type = INPUT_EV_REL, .code = INPUT_REL_Y, .value = -3, .sync = true, .ends_packet = true},
    {.type = INPUT_EV_REL, .code = INPUT_REL_WHEEL, .value = 1, .sync = true, .ends_packet = true},
    {.type = INPUT_EV_KEY, .code = INPUT_BTN_0, .value = 1, .sync = true, .ends_packet = true},
    {.type = INPUT_EV_REL, .code = INPUT_REL_X, .value = -2},
    {.type = INPUT_EV_REL, .code = INPUT_REL_Y, .value = 4},
    /* Batch full before its sync, the packet ends without one. */
    {.type = INPUT_EV_REL, .code = INPUT_REL_HWHEEL, .value = -1, .ends_packet = true},
    {.type = INPUT_EV_REL, .code = INPUT_REL_X, .value = WIDE_MOTION, .sync = true,
     .ends_packet = true},
    {.type = INPUT_EV_KEY, .code = INPUT_BTN_0, .value = 0, .sync = true, .ends_packet = true},
};

static size_t next_expected;

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
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

static void pass_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("PASS: all %u pointer events replayed in order, nothing extra\n",
           (unsigned int)ARRAY_SIZE(events));
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(pass_work, pass_fn);

static bool event_matches(const struct input_event *event, const struct pointer_event *want) {
    if (event->type != want->type || event->code != want->code) {
        return false;
    }
    return event->value == want->value && (bool)event->sync == want->sync;
}

static void pointer_observer(struct input_event *event, void *user_data) {
    ARG_UNUSED(user_data);
    if (next_expected == ARRAY_SIZE(events)) {
        printk("FAIL: extra event type %u code 0x%03x value %d\n", event->type, event->code,
               (int)event->value);
        exit(1);
    }
    const struct pointer_event *want = &events[next_expected];
    if (!event_matches(event, want)) {
        printk("FAIL: event %u of %u, expected type %u code 0x%03x value %d sync %d, "
               "got type %u code 0x%03x value %d sync %d\n",
               (unsigned int)(next_expected + 1), (unsigned int)ARRAY_SIZE(events), want->type,
               want->code, (int)want->value, want->sync, event->type, event->code,
               (int)event->value, event->sync);
        exit(1);
    }
    next_expected++;
    if (next_expected == ARRAY_SIZE(events)) {
        k_work_reschedule(&pass_work, K_MSEC(SETTLE_MS));
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(split_pointer)), pointer_observer, NULL);

static size_t encode_event(const struct pointer_event *event, uint8_t *out, size_t room) {
    const struct zmk_split_transport_peripheral_event split_event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data.input_event = {.reg = POINTER_REG,
                             .type = event->type,
                             .code = event->code,
                             .value = event->value,
                             .sync = event->sync},
    };
    size_t length = esb_wire_encode_event(out, room, &split_event);
    if (length == 0) {
        printk("FAIL: script event does not encode\n");
        exit(1);
    }
    return length;
}

static void send_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t packet[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
    size_t length = 0;
    for (size_t index = 0; index < ARRAY_SIZE(events); index++) {
        length += encode_event(&events[index], &packet[length], sizeof(packet) - length);
        if (events[index].ends_packet) {
            central_ingest_packet(PIPE, packet, length);
            length = 0;
        }
    }
}
static K_WORK_DELAYABLE_DEFINE(send_work, send_fn);

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: replay stopped at event %u of %u\n", (unsigned int)(next_expected + 1),
           (unsigned int)ARRAY_SIZE(events));
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&send_work, K_MSEC(SEND_DELAY_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
