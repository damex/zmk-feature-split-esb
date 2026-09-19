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
#include "spsc_latch.h"

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

#define RELAY_PIPE_BIT(node) \
    +(DT_ENUM_HAS_VALUE(node, role, relay) ? (1u << DT_PROP(node, pipe)) : 0u)
#define RELAY_PIPE_MASK (0u DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, RELAY_PIPE_BIT))

#define SELF_PIPE_BIT(node) \
    +(DT_ENUM_HAS_VALUE(node, role, self) ? (1u << DT_PROP(node, pipe)) : 0u)
#define SELF_PIPE_MASK (0u DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, SELF_PIPE_BIT))

bool esb_link_pipe_is_relay(uint8_t pipe) {
    return (RELAY_PIPE_MASK & (1u << pipe)) != 0u;
}

bool esb_link_pipe_is_self(uint8_t pipe) {
    return (SELF_PIPE_MASK & (1u << pipe)) != 0u;
}

uint8_t esb_link_source_ids(uint8_t *out_ids) {
    __ASSERT_NO_MSG(out_ids != NULL);
    uint8_t out_count = 0;
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        if (esb_link_pipe_is_self(pipe)) {
            continue;
        }
        out_ids[out_count++] = pipe;
    }
    return out_count;
}

BUILD_ASSERT(ESB_LINK_CONTROL_MAX_LENGTH <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "control latch does not fit one ESB payload");

#define CONTROL_LATCH_SLOTS 2
#define REPLACE_LATCH_SLOTS 2

struct control_payload {
    uint8_t data[ESB_LINK_CONTROL_MAX_LENGTH];
    uint8_t length;
};

struct reply_payload {
    uint8_t data[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
    uint8_t length;
};

static struct control_payload
    control_pool[REPLY_PIPE_COUNT][ESB_LINK_CONTROL_COUNT][CONTROL_LATCH_SLOTS];
static struct spsc_latch control_latch[REPLY_PIPE_COUNT][ESB_LINK_CONTROL_COUNT];

static struct reply_payload replace_pool[REPLY_PIPE_COUNT][REPLACE_LATCH_SLOTS];
static struct spsc_latch replace_latch[REPLY_PIPE_COUNT];

static int reply_latches_init(void) {
    for (size_t pipe = 0; pipe < REPLY_PIPE_COUNT; pipe++) {
        for (size_t kind = 0; kind < ESB_LINK_CONTROL_COUNT; kind++) {
            control_latch[pipe][kind].slot_count = CONTROL_LATCH_SLOTS;
        }
        replace_latch[pipe].slot_count = REPLACE_LATCH_SLOTS;
    }
    return 0;
}
SYS_INIT(reply_latches_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

int esb_link_latch_control(uint8_t pipe, enum esb_link_control kind, const uint8_t *data,
                           size_t length) {
    __ASSERT_NO_MSG(data != NULL);
    if (pipe >= REPLY_PIPE_COUNT || kind >= ESB_LINK_CONTROL_COUNT) {
        return -EINVAL;
    }
    if (length == 0 || length > ESB_LINK_CONTROL_MAX_LENGTH) {
        return -EMSGSIZE;
    }
    uint8_t index = spsc_latch_claim(&control_latch[pipe][kind]);
    struct control_payload *slot = &control_pool[pipe][kind][index];
    memcpy(slot->data, data, length);
    slot->length = (uint8_t)length;
    spsc_latch_publish(&control_latch[pipe][kind], slot);
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

int esb_link_replace_reply(uint8_t pipe, const uint8_t *data, size_t length) {
    if (pipe >= REPLY_PIPE_COUNT) {
        return -EINVAL;
    }
    if (length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    uint8_t index = spsc_latch_claim(&replace_latch[pipe]);
    struct reply_payload *slot = &replace_pool[pipe][index];
    if (length > 0) {
        memcpy(slot->data, data, length);
    }
    slot->length = (uint8_t)length;
    spsc_latch_publish(&replace_latch[pipe], slot);
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
        struct control_payload *slot = spsc_latch_peek(&control_latch[pipe][kind]);
        if (slot == NULL) {
            continue;
        }
        struct esb_payload payload = {0};
        payload.pipe = pipe;
        payload.length = slot->length;
        memcpy(payload.data, slot->data, slot->length);
        if (esb_write_payload(&payload) == 0) {
            (void)spsc_latch_release(&control_latch[pipe][kind], slot);
        }
        return true;
    }
    return false;
}

static bool write_pending_replace(uint8_t pipe) {
    struct reply_payload *slot = spsc_latch_peek(&replace_latch[pipe]);
    if (slot == NULL) {
        return false;
    }
    struct esb_payload payload = {0};
    payload.pipe = pipe;
    payload.length = slot->length;
    if (slot->length > 0) {
        memcpy(payload.data, slot->data, slot->length);
    }
    if (esb_write_payload(&payload) == 0) {
        (void)spsc_latch_release(&replace_latch[pipe], slot);
    }
    return true;
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
        if (write_pending_replace(pipe)) {
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
