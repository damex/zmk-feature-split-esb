// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake esb_link_send under input batching.
 * Exits 0 once every check passes, 1 at the first failing one.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/types.h>

#include "esb_batch.h"
#include "esb_link.h"
#include "esb_wire.h"
#include "mock.h"

#define PACKETS_MAX 4

struct sent_packet {
    uint8_t data[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
    size_t length;
    bool ack;
};

static struct sent_packet sent[PACKETS_MAX];
static size_t sent_count;
static int send_result;
static struct esb_batch batch;

int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    if (sent_count < PACKETS_MAX) {
        memcpy(sent[sent_count].data, data, length);
        sent[sent_count].length = length;
        sent[sent_count].ack = ack;
    }
    sent_count++;
    return send_result;
}

static struct zmk_split_transport_peripheral_event pointer_event(uint16_t code, int32_t value,
                                                                 bool sync) {
    return (struct zmk_split_transport_peripheral_event){
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data.input_event = {.type = INPUT_EV_REL, .code = code, .value = value, .sync = sync},
    };
}

static bool packet_holds(const struct sent_packet *packet,
                         const struct zmk_split_transport_peripheral_event *events, size_t count) {
    size_t offset = 0;
    for (size_t index = 0; index < count; index++) {
        struct zmk_split_transport_peripheral_event decoded;
        size_t consumed =
            esb_wire_decode_event(&packet->data[offset], packet->length - offset, &decoded);
        if (consumed == 0 || memcmp(&decoded, &events[index], sizeof(decoded)) != 0) {
            return false;
        }
        offset += consumed;
    }
    return offset == packet->length;
}

static void report(const struct zmk_split_transport_peripheral_event *event, bool wants_ack) {
    mock_check(esb_batch_report_event(&batch, event, wants_ack) == 0, "report accepted");
}

static void check_sync_flushes(void) {
    sent_count = 0;
    const struct zmk_split_transport_peripheral_event events[] = {
        pointer_event(INPUT_REL_X, 3, false),
        pointer_event(INPUT_REL_Y, -4, true),
    };
    report(&events[0], false);
    mock_check(sent_count == 0, "no packet before the sync event");
    report(&events[1], false);
    mock_check(sent_count == 1, "sync event flushes one packet");
    mock_check(packet_holds(&sent[0], events, ARRAY_SIZE(events)),
               "packet holds both axes in order");
    mock_check(!sent[0].ack, "lossy batch sends without ack");
}

static void check_ack_sticks(void) {
    sent_count = 0;
    const struct zmk_split_transport_peripheral_event events[] = {
        pointer_event(INPUT_REL_X, 1, false),
        pointer_event(INPUT_REL_WHEEL, 1, false),
        pointer_event(INPUT_REL_Y, 1, true),
    };
    report(&events[0], false);
    report(&events[1], true);
    report(&events[2], false);
    mock_check(sent_count == 1, "ack batch flushes one packet");
    mock_check(sent[0].ack, "one ack-wanting event makes the batch acked");
}

static void check_flush_resets(void) {
    sent_count = 0;
    const struct zmk_split_transport_peripheral_event event = pointer_event(INPUT_REL_X, 9, true);
    report(&event, false);
    mock_check(sent_count == 1, "next batch flushes on its own sync");
    mock_check(packet_holds(&sent[0], &event, 1), "flushed batch starts empty");
    mock_check(!sent[0].ack, "ack request does not carry into the next batch");
}

static void check_full_batch_flushes(void) {
    sent_count = 0;
    struct zmk_split_transport_peripheral_event events[ESB_BATCH_MAX];
    for (size_t index = 0; index < ARRAY_SIZE(events); index++) {
        events[index] = pointer_event(INPUT_REL_X, (int32_t)index, false);
    }
    for (size_t index = 0; index + 1 < ARRAY_SIZE(events); index++) {
        report(&events[index], false);
    }
    mock_check(sent_count == 0, "no packet before the batch fills");
    report(&events[ARRAY_SIZE(events) - 1], false);
    mock_check(sent_count == 1, "full batch flushes without a sync event");
    mock_check(packet_holds(&sent[0], events, ARRAY_SIZE(events)), "full packet holds every event");
}

static void check_key_event_flushes(void) {
    sent_count = 0;
    const struct zmk_split_transport_peripheral_event motion = pointer_event(INPUT_REL_X, 2, false);
    const struct zmk_split_transport_peripheral_event button = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data.input_event = {.type = INPUT_EV_KEY, .code = INPUT_BTN_0, .value = 1, .sync = false},
    };
    const struct zmk_split_transport_peripheral_event events[] = {motion, button};
    report(&motion, false);
    report(&button, true);
    mock_check(sent_count == 1, "key event flushes without a sync event");
    mock_check(packet_holds(&sent[0], events, ARRAY_SIZE(events)),
               "key event packet holds the motion before it");
}

static void check_empty_flush(void) {
    sent_count = 0;
    mock_check(esb_batch_flush(&batch) == 0, "empty flush succeeds");
    mock_check(sent_count == 0, "empty flush sends nothing");
}

static void check_send_error(void) {
    sent_count = 0;
    send_result = -ENOMEM;
    const struct zmk_split_transport_peripheral_event event = pointer_event(INPUT_REL_X, 1, true);
    mock_check(esb_batch_report_event(&batch, &event, false) == -ENOMEM,
               "link send error reaches the caller");
    send_result = 0;
}

static int test_batch_init(void) {
    check_sync_flushes();
    check_ack_sticks();
    check_flush_resets();
    check_full_batch_flushes();
    check_key_event_flushes();
    check_empty_flush();
    check_send_error();
    printk("PASS: all %u input batch checks\n", (unsigned int)mock_checks_passed());
    exit(0);
    return 0;
}
SYS_INIT(test_batch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
