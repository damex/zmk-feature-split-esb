// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Central half of the ESB radio layer: source ids and the ACK reverse channel.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"

#define ESB_PERIPHERALS DT_INST_CHILD(0, peripherals)

/* Per pipe: offline peripheral backs up only its own replies. */
#define REPLY_QUEUE_DEFINE(node)                                                                    \
    static struct k_msgq reply_queue_##node;                                                       \
    static char reply_buffer_##node[DT_PROP(node, reply_queue_depth) *                              \
                                    sizeof(struct esb_link_packet)] __aligned(4);
DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, REPLY_QUEUE_DEFINE)

#define REPLY_QUEUE_PTR(node) [DT_PROP(node, pipe)] = &reply_queue_##node,
static struct k_msgq *const reply_queue[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, REPLY_QUEUE_PTR)
};
#define REPLY_PIPE_COUNT ARRAY_SIZE(reply_queue)

#define REPLY_QUEUE_INIT(node)                                                                      \
    k_msgq_init(&reply_queue_##node, reply_buffer_##node, sizeof(struct esb_link_packet),           \
                DT_PROP(node, reply_queue_depth));
static int reply_queue_init(void) {
    DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, REPLY_QUEUE_INIT)
    return 0;
}
SYS_INIT(reply_queue_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

uint8_t esb_link_source_ids(uint8_t *out_ids) {
    __ASSERT_NO_MSG(out_ids != NULL);
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        out_ids[pipe] = pipe;
    }
    return esb_link_pipe_count;
}

BUILD_ASSERT(ESB_LINK_CONTROL_MAX_LENGTH <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "control latch does not fit one ESB payload");

struct control_slot {
    uint8_t data[ESB_LINK_CONTROL_MAX_LENGTH];
    uint8_t length;
    bool pending;
};
static struct control_slot control_slots[REPLY_PIPE_COUNT][ESB_LINK_CONTROL_COUNT];

int esb_link_latch_control(uint8_t pipe, enum esb_link_control kind, const uint8_t *data,
                           size_t length) {
    __ASSERT_NO_MSG(data != NULL);
    if (pipe >= REPLY_PIPE_COUNT || kind >= ESB_LINK_CONTROL_COUNT) {
        return -EINVAL;
    }
    if (length == 0 || length > ESB_LINK_CONTROL_MAX_LENGTH) {
        return -EMSGSIZE;
    }
    struct control_slot *slot = &control_slots[pipe][kind];
    unsigned int key = irq_lock();
    memcpy(slot->data, data, length);
    slot->length = (uint8_t)length;
    slot->pending = true;
    irq_unlock(key);
    return 0;
}

int esb_link_stage_reply(uint8_t pipe, const uint8_t *data, size_t length) {
    if (pipe >= REPLY_PIPE_COUNT) {
        return -EINVAL;
    }
    if (length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    struct esb_link_packet packet = {0};
    packet.pipe = pipe;
    packet.length = (uint8_t)length;
    if (length > 0) {
        memcpy(packet.data, data, length);
    }
    if (k_msgq_put(reply_queue[pipe], &packet, K_NO_WAIT) != 0) {
        return -ENOBUFS;
    }
    return 0;
}

int esb_link_role_start(void) {
    return esb_start_rx();
}

void esb_link_set_idle(bool idle) {
    ARG_UNUSED(idle);
}

static bool write_pending_control(uint8_t pipe) {
    for (size_t kind = 0; kind < ESB_LINK_CONTROL_COUNT; kind++) {
        struct control_slot *slot = &control_slots[pipe][kind];
        if (!slot->pending) {
            continue;
        }
        struct esb_payload payload = {0};
        payload.pipe = pipe;
        payload.length = slot->length;
        memcpy(payload.data, slot->data, slot->length);
        if (esb_write_payload(&payload) == 0) {
            slot->pending = false;
        }
        return true;
    }
    return false;
}

/* ISR-only, so esb_write_payload has a single caller context, no lock.
 * ACK FIFO is shared across pipes: reply only for a pipe that just RXed, one write
 * per RX, so an idle pipe never head-of-line blocks others and a dying one leaks a
 * single slot. */
void esb_link_role_rx_done(uint8_t pipes_seen) {
    for (uint8_t pipe = 0; pipe < REPLY_PIPE_COUNT; pipe++) {
        if ((pipes_seen & BIT(pipe)) == 0) {
            continue;
        }
        if (write_pending_control(pipe)) {
            continue;
        }
        struct esb_link_packet packet;
        if (k_msgq_peek(reply_queue[pipe], &packet) != 0) {
            continue;
        }
        struct esb_payload payload = {0};
        payload.pipe = packet.pipe;
        payload.length = packet.length;
        if (packet.length > 0) {
            memcpy(payload.data, packet.data, packet.length);
        }
        if (esb_write_payload(&payload) == 0) {
            (void)k_msgq_get(reply_queue[pipe], &packet, K_NO_WAIT);
        }
    }
}
