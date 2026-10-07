// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB on a wire relay half.
 * Exits 0 once own events leave on own pipe and wire peer frames on the peer pipe, each in order.
 * Exits 1 on a wrong packet or at deadline.
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

#include <zmk/split/transport/types.h>

#include <esb.h>

#include "esb_wire.h"
#include "hop.h"
#include "mock.h"
#include "mock_wire.h"

#define SELF_PIPE DT_PROP(DT_CHOSEN(zmk_esb_self), pipe)
#define PEER_PIPE DT_PROP(DT_CHOSEN(zmk_esb_wire_peer), pipe)
#define WIRE_INJECT_DELAY_MS 50
#define VERDICT_DEADLINE_MS 1000
#define OWN_POSITION 0
#define PEER_POSITION 5

static const struct zmk_split_transport_peripheral_event own_events[] = {
    {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = OWN_POSITION, .pressed = true},
    },
    {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = OWN_POSITION, .pressed = false},
    },
};

static const struct zmk_split_transport_peripheral_event peer_events[] = {
    {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = PEER_POSITION, .pressed = true},
    },
    {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = PEER_POSITION, .pressed = false},
    },
};

struct pipe_stream {
    uint8_t pipe;
    const struct zmk_split_transport_peripheral_event *events;
    size_t count;
    size_t next;
};

static struct pipe_stream streams[] = {
    {.pipe = SELF_PIPE, .events = own_events, .count = ARRAY_SIZE(own_events)},
    {.pipe = PEER_PIPE, .events = peer_events, .count = ARRAY_SIZE(peer_events)},
};

static struct pipe_stream *stream_for_pipe(uint8_t pipe) {
    for (size_t index = 0; index < ARRAY_SIZE(streams); index++) {
        if (streams[index].pipe == pipe) {
            return &streams[index];
        }
    }
    return NULL;
}

static bool streams_complete(void) {
    for (size_t index = 0; index < ARRAY_SIZE(streams); index++) {
        if (streams[index].next < streams[index].count) {
            return false;
        }
    }
    return true;
}

static void check_payload(const struct esb_payload *payload) {
    struct pipe_stream *stream = stream_for_pipe(payload->pipe);
    if (stream == NULL) {
        printk("FAIL: packet on unexpected pipe %u\n", payload->pipe);
        exit(1);
    }
    if (stream->next == stream->count) {
        printk("FAIL: extra packet on pipe %u\n", payload->pipe);
        exit(1);
    }
    uint8_t expected[ESB_WIRE_MAX_EVENT_SIZE];
    size_t expected_length =
        esb_wire_encode_event(expected, sizeof(expected), &stream->events[stream->next]);
    if (payload->length != expected_length || memcmp(payload->data, expected, expected_length) != 0) {
        printk("FAIL: pipe %u step %u of %u, expected", payload->pipe,
               (unsigned int)(stream->next + 1), (unsigned int)stream->count);
        mock_print_bytes(expected, expected_length);
        printk("FAIL: got");
        mock_print_bytes(payload->data, payload->length);
        exit(1);
    }
    if (payload->noack) {
        printk("FAIL: pipe %u step %u sent without ack\n", payload->pipe,
               (unsigned int)(stream->next + 1));
        exit(1);
    }
    stream->next++;
    if (streams_complete()) {
        printk("PASS: own events on pipe %u, wire peer frames on pipe %u, each in order\n",
               SELF_PIPE, PEER_PIPE);
        exit(0);
    }
}

int esb_write_payload(const struct esb_payload *payload) {
    check_payload(payload);
    return 0;
}

void hop_restore(void) {
}

uint8_t hop_link_cost_x10(void) {
    return 0;
}

void hop_note_data_sent(void) {
}

static void wire_inject_fn(struct k_work *work) {
    ARG_UNUSED(work);
    for (size_t index = 0; index < ARRAY_SIZE(peer_events); index++) {
        uint8_t payload[ESB_WIRE_MAX_EVENT_SIZE];
        size_t length = esb_wire_encode_event(payload, sizeof(payload), &peer_events[index]);
        mock_wire_rx_inject(payload, length);
    }
}
static K_WORK_DELAYABLE_DEFINE(wire_inject_work, wire_inject_fn);

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    for (size_t index = 0; index < ARRAY_SIZE(streams); index++) {
        printk("FAIL: pipe %u stopped at step %u of %u\n", streams[index].pipe,
               (unsigned int)(streams[index].next + 1), (unsigned int)streams[index].count);
    }
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    k_work_reschedule(&wire_inject_work, K_MSEC(WIRE_INJECT_DELAY_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
