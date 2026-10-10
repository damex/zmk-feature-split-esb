// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c on a mouse with lossy X/Y over a lossy channel.
 * One fire-and-forget packet in DROP_EVERY never reaches the central.
 * Central side reads totals and keepalive totals as the module's central does.
 * Exits 0 once the motion reaching the central, past its first baseline,
 * adds up to the motion the mouse sent.
 * Exits 1 when the central comes up short or a packet does not decode.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/types.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "esb_motion_total.h"
#include "esb_wire.h"
#include "mock_esb.h"

#define POINTER_DEVICE DEVICE_DT_GET(DT_NODELABEL(pointer_input))
#define HOP_WINDOW_MS DT_INST_PROP(0, hop_window_ms)
#define MOTION_START_MS 50
#define MOTION_PERIOD_MS 4
#define MOTION_STEPS 200
#define DROP_EVERY 5
#define FIRST_TRY_ATTEMPTS 1
#define HEAL_MS (2 * DT_INST_PROP(0, idle_keepalive_ms))
#define VERDICT_MS (MOTION_START_MS + MOTION_STEPS * (MOTION_PERIOD_MS + 1) + HEAL_MS)

struct central_axis {
    uint16_t code;
    struct esb_motion_track track;
    int32_t delivered;
    int32_t baseline;
    bool baseline_seen;
};

static struct central_axis axes[] = {
    {.code = INPUT_REL_X},
    {.code = INPUT_REL_Y},
};

static atomic_t sent_steps;
static atomic_t lossy_packets;
static atomic_t dropped_packets;
static uint32_t motion_heard_ms;

static void tx_success_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_esb_tx_success(FIRST_TRY_ATTEMPTS);
}
static K_WORK_DEFINE(tx_success_work, tx_success_fn);

static struct central_axis *axis_for(uint16_t code) {
    for (size_t index = 0; index < ARRAY_SIZE(axes); index++) {
        if (axes[index].code == code) {
            return &axes[index];
        }
    }
    return NULL;
}

static void add_total(struct central_axis *axis, int32_t total) {
    if (!axis->baseline_seen) {
        axis->baseline = total;
        axis->baseline_seen = true;
    }
    int32_t delta = 0;
    if (esb_motion_track_delta(&axis->track, total, &delta)) {
        axis->delivered += delta;
    }
}

static void add_motion(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT ||
        event->data.input_event.type != INPUT_EV_REL) {
        return;
    }
    struct central_axis *axis = axis_for(event->data.input_event.code);
    if (axis == NULL) {
        return;
    }
    if ((event->data.input_event.sync & ESB_WIRE_INPUT_TOTAL) == 0) {
        axis->delivered += event->data.input_event.value;
        return;
    }
    motion_heard_ms = k_uptime_get_32();
    add_total(axis, event->data.input_event.value);
}

static void deliver_events(const struct esb_payload *payload) {
    size_t offset = 0;
    while (offset < payload->length) {
        struct zmk_split_transport_peripheral_event event = {0};
        size_t consumed =
            esb_wire_decode_event(&payload->data[offset], payload->length - offset, &event);
        if (consumed == 0) {
            printk("FAIL: packet of %u bytes does not decode at offset %u\n",
                   (unsigned int)payload->length, (unsigned int)offset);
            exit(1);
        }
        add_motion(&event);
        offset += consumed;
    }
}

static void deliver_keepalive(const uint8_t *keepalive) {
    if (!esb_motion_quiet_enough(k_uptime_get_32() - motion_heard_ms, HOP_WINDOW_MS)) {
        return;
    }
    for (uint8_t index = 0; index < esb_keepalive_motion_count(keepalive); index++) {
        struct esb_keepalive_motion_total motion = esb_keepalive_motion_total_at(keepalive, index);
        struct central_axis *axis = axis_for(motion.code);
        int32_t delta = 0;
        if (axis != NULL && esb_motion_track_short_delta(&axis->track, motion.short_total, &delta)) {
            axis->delivered += delta;
        }
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    k_work_submit(&tx_success_work);
    if (esb_keepalive_matches(payload->data, payload->length)) {
        deliver_keepalive(payload->data);
        return 0;
    }
    if (payload->noack && (atomic_inc(&lossy_packets) + 1) % DROP_EVERY == 0) {
        atomic_inc(&dropped_packets);
        return 0;
    }
    deliver_events(payload);
    return 0;
}

static void motion_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(motion_work, motion_fn);

static void motion_fn(struct k_work *work) {
    ARG_UNUSED(work);
    input_report_rel(POINTER_DEVICE, INPUT_REL_X, 1, false, K_NO_WAIT);
    input_report_rel(POINTER_DEVICE, INPUT_REL_Y, 1, true, K_NO_WAIT);
    if (atomic_inc(&sent_steps) + 1 < MOTION_STEPS) {
        k_work_reschedule(&motion_work, K_MSEC(MOTION_PERIOD_MS));
    }
}

static void check_axis(const struct central_axis *axis, int32_t sent) {
    if (axis->delivered + axis->baseline == sent) {
        return;
    }
    printk("FAIL: central got %d of axis 0x%02x past a %d baseline, mouse sent %d, "
           "%u of %u lossy packets dropped\n",
           (int)axis->delivered, (unsigned int)axis->code, (int)axis->baseline, (int)sent,
           (unsigned int)atomic_get(&dropped_packets), (unsigned int)atomic_get(&lossy_packets));
    exit(1);
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    int32_t sent = (int32_t)atomic_get(&sent_steps);
    for (size_t index = 0; index < ARRAY_SIZE(axes); index++) {
        check_axis(&axes[index], sent);
    }
    printk("PASS: central got all %d steps per axis with %u of %u lossy packets dropped\n",
           (int)sent, (unsigned int)atomic_get(&dropped_packets),
           (unsigned int)atomic_get(&lossy_packets));
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&motion_work, K_MSEC(MOTION_START_MS));
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
