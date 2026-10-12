// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Fake NCS ESB PRX ACK FIFO, queued ACK payloads go out and leave as NCS does. */

#include "mock_esb_ack_fifo.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "mock.h"
#include "mock_esb.h"

struct pipe_queue {
    struct esb_payload payloads[CONFIG_ESB_TX_FIFO_SIZE];
    size_t head;
    size_t count;
    bool head_sent;
};

static struct pipe_queue queues[CONFIG_ESB_PIPE_COUNT];
static size_t slots_used;
static size_t slots_peak;
static size_t full_refusals;

int esb_write_payload(const struct esb_payload *payload) {
    if (!mock_esb_initialized()) {
        return -EACCES;
    }
    if (payload == NULL) {
        return -EINVAL;
    }
    if (payload->length == 0 || payload->length > CONFIG_ESB_MAX_PAYLOAD_LENGTH) {
        return -EMSGSIZE;
    }
    if (slots_used >= CONFIG_ESB_TX_FIFO_SIZE) {
        full_refusals++;
        return -ENOMEM;
    }
    if (payload->pipe >= ARRAY_SIZE(queues)) {
        return -EINVAL;
    }
    struct pipe_queue *queue = &queues[payload->pipe];
    queue->payloads[(queue->head + queue->count) % CONFIG_ESB_TX_FIFO_SIZE] = *payload;
    queue->count++;
    slots_used++;
    slots_peak = MAX(slots_peak, slots_used);
    return 0;
}

static bool answer_with_ack(uint8_t pipe) {
    struct pipe_queue *queue = &queues[pipe];
    bool head_delivered = queue->head_sent;
    if (head_delivered) {
        queue->head = (queue->head + 1) % CONFIG_ESB_TX_FIFO_SIZE;
        queue->count--;
        slots_used--;
    }
    queue->head_sent = queue->count != 0;
    return head_delivered;
}

static const struct esb_payload *ack_payload(uint8_t pipe) {
    const struct pipe_queue *queue = &queues[pipe];
    if (!queue->head_sent) {
        return NULL;
    }
    return &queue->payloads[queue->head];
}

const struct esb_payload *mock_esb_ack_fifo_receive(const struct esb_payload *payload) {
    mock_check(payload->pipe < ARRAY_SIZE(queues), "received pipe is an ESB pipe");
    const struct esb_payload *ack = NULL;
    if (payload->noack == 0) {
        if (answer_with_ack(payload->pipe)) {
            mock_esb_tx_success(0);
        }
        ack = ack_payload(payload->pipe);
    }
    mock_esb_rx_push(payload);
    mock_esb_rx_raise();
    return ack;
}

size_t mock_esb_ack_fifo_full_refusals(void) {
    return full_refusals;
}

size_t mock_esb_ack_fifo_peak_slots(void) {
    return slots_peak;
}

size_t mock_esb_ack_fifo_queued(uint8_t pipe) {
    if (pipe >= ARRAY_SIZE(queues)) {
        return 0;
    }
    return queues[pipe].count;
}
