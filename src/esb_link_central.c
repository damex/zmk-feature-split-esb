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

#include "esb_hid_relay_pointer.h"
#include "esb_link.h"
#include "esb_link_internal.h"
#include "esb_stats.h"
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

#define PIPE_HAS_ROLE(node, value) [DT_PROP(node, pipe)] = DT_ENUM_HAS_VALUE(node, role, value),
static const bool relay_pipes[ESB_LINK_PIPE_MAX] = {
    DT_FOREACH_CHILD_STATUS_OKAY_VARGS(ESB_PERIPHERALS, PIPE_HAS_ROLE, relay)
};
static const bool self_pipes[ESB_LINK_PIPE_MAX] = {
    DT_FOREACH_CHILD_STATUS_OKAY_VARGS(ESB_PERIPHERALS, PIPE_HAS_ROLE, self)
};

bool esb_link_pipe_is_relay(uint8_t pipe) {
    if (pipe >= ARRAY_SIZE(relay_pipes)) {
        return false;
    }
    return relay_pipes[pipe];
}

bool esb_link_pipe_is_self(uint8_t pipe) {
    if (pipe >= ARRAY_SIZE(self_pipes)) {
        return false;
    }
    return self_pipes[pipe];
}

uint8_t esb_link_source_ids(uint8_t *out_ids) {
    __ASSERT_NO_MSG(out_ids != NULL);
    uint8_t out_count = 0;
    for (uint8_t pipe = 0; pipe < esb_link_pipe_count; pipe++) {
        if (esb_link_pipe_is_self(pipe) || esb_link_pipe_is_relay(pipe)) {
            continue;
        }
        out_ids[out_count++] = pipe;
    }
    return out_count;
}

BUILD_ASSERT(ESB_LINK_CONTROL_MAX_LENGTH <= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD,
             "control latch does not fit one ESB payload");

#define REPLIES_PER_PIPE_MAX 2
BUILD_ASSERT(CONFIG_ESB_TX_FIFO_SIZE >= REPLIES_PER_PIPE_MAX * REPLY_PIPE_COUNT,
             "set CONFIG_ESB_TX_FIFO_SIZE to twice the peripheral count or more in your .conf");

#define CONTROL_LATCH_SLOTS 2

struct control_payload {
    uint8_t data[ESB_LINK_CONTROL_MAX_LENGTH];
    uint8_t length;
};

static struct control_payload
    control_pool[REPLY_PIPE_COUNT][ESB_LINK_CONTROL_COUNT][CONTROL_LATCH_SLOTS];
static struct spsc_latch control_latch[REPLY_PIPE_COUNT][ESB_LINK_CONTROL_COUNT];

#define IDLE_REPLY_SLOTS 2

struct idle_reply {
    uint8_t data[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
    uint8_t length;
    atomic_val_t replies_staged;
};

static struct idle_reply idle_reply_pool[REPLY_PIPE_COUNT][IDLE_REPLY_SLOTS];
static struct spsc_latch idle_reply_latch[REPLY_PIPE_COUNT];
static atomic_t replies_staged[REPLY_PIPE_COUNT];

static int reply_latches_init(void) {
    for (size_t pipe = 0; pipe < REPLY_PIPE_COUNT; pipe++) {
        for (size_t kind = 0; kind < ESB_LINK_CONTROL_COUNT; kind++) {
            control_latch[pipe][kind].slot_count = CONTROL_LATCH_SLOTS;
        }
        idle_reply_latch[pipe].slot_count = IDLE_REPLY_SLOTS;
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
    if (length == 0 || length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    struct esb_link_packet packet = {0};
    packet.pipe = pipe;
    packet.length = (uint8_t)length;
    memcpy(packet.data, data, length);
    if (k_msgq_put(reply_queue[pipe], &packet, K_NO_WAIT) != 0) {
        return -ENOBUFS;
    }
    atomic_inc(&replies_staged[pipe]);
    return 0;
}

int esb_link_latch_idle_reply(uint8_t pipe, const uint8_t *data, size_t length) {
    __ASSERT_NO_MSG(data != NULL);
    if (pipe >= REPLY_PIPE_COUNT) {
        return -EINVAL;
    }
    if (length == 0 || length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    uint8_t index = spsc_latch_claim(&idle_reply_latch[pipe]);
    struct idle_reply *slot = &idle_reply_pool[pipe][index];
    memcpy(slot->data, data, length);
    slot->length = (uint8_t)length;
    slot->replies_staged = atomic_get(&replies_staged[pipe]);
    spsc_latch_publish(&idle_reply_latch[pipe], slot);
    return 0;
}

int esb_link_role_start(void) {
    return esb_start_rx();
}

void esb_link_set_idle(bool idle) {
    ARG_UNUSED(idle);
}

static int write_reply(const struct esb_payload *payload) {
    int error = esb_write_payload(payload);
    if (error != 0) {
        esb_stat_add(ESB_STAT_REPLY_REFUSED, 1);
        return error;
    }
    esb_stat_add(ESB_STAT_REPLY_WRITTEN, 1);
    esb_stat_reply_written(payload->pipe);
    return error;
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
        if (write_reply(&payload) == 0) {
            (void)spsc_latch_release(&control_latch[pipe][kind], slot);
        }
        return true;
    }
    return false;
}

static size_t append_pointer(uint8_t pipe, struct esb_payload *payload, bool changes_queued) {
    if (!esb_link_pipe_is_relay(pipe)) {
        return 0;
    }
    size_t room = CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD - payload->length;
    size_t length =
        esb_hid_relay_pointer_take(&payload->data[payload->length], room, changes_queued);
    payload->length = (uint8_t)(payload->length + length);
    return length;
}

static void commit_pointer(const struct esb_payload *payload, size_t offset, bool has_pointer) {
    if (has_pointer) {
        esb_hid_relay_pointer_sent(&payload->data[offset]);
    }
}

struct packed_replies {
    uint32_t taken;
    size_t pointer_offset;
    bool has_pointer;
};

static size_t queued_room(uint8_t pipe) {
    if (!esb_link_pipe_is_relay(pipe) || !esb_hid_relay_pointer_motion_pending()) {
        return CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD;
    }
    return CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD - ESB_HID_RELAY_POINTER_LENGTH;
}

static struct packed_replies pack_queued_replies(uint8_t pipe, struct esb_payload *payload) {
    struct packed_replies packed = {0};
    size_t room = queued_room(pipe);
    struct esb_link_packet packet;
    while (k_msgq_peek_at(reply_queue[pipe], &packet, packed.taken) == 0) {
        if (payload->length + packet.length > room) {
            break;
        }
        uint8_t *reply = &payload->data[payload->length];
        memcpy(reply, packet.data, packet.length);
        if (esb_link_pipe_is_relay(pipe) && esb_hid_relay_pointer_stamp(reply, packet.length)) {
            packed.pointer_offset = payload->length;
            packed.has_pointer = true;
        }
        payload->length = (uint8_t)(payload->length + packet.length);
        packed.taken++;
        /* Relay replies are whole HID reports the dongle splits again, commands are not. */
        if (!esb_link_pipe_is_relay(pipe)) {
            break;
        }
    }
    return packed;
}

static bool write_queued_replies(uint8_t pipe) {
    struct esb_payload payload = {0};
    payload.pipe = pipe;
    struct packed_replies packed = pack_queued_replies(pipe, &payload);
    if (packed.taken == 0) {
        return false;
    }
    struct esb_link_packet packet;
    if (!packed.has_pointer) {
        bool changes_queued = k_msgq_peek_at(reply_queue[pipe], &packet, packed.taken) == 0;
        packed.pointer_offset = payload.length;
        packed.has_pointer = append_pointer(pipe, &payload, changes_queued) != 0;
    }
    if (write_reply(&payload) == 0) {
        for (uint32_t index = 0; index < packed.taken; index++) {
            (void)k_msgq_get(reply_queue[pipe], &packet, K_NO_WAIT);
        }
        commit_pointer(&payload, packed.pointer_offset, packed.has_pointer);
    }
    return true;
}

static struct idle_reply *fresh_idle_reply(uint8_t pipe) {
    struct idle_reply *slot = spsc_latch_peek(&idle_reply_latch[pipe]);
    if (slot != NULL && slot->replies_staged != atomic_get(&replies_staged[pipe])) {
        (void)spsc_latch_release(&idle_reply_latch[pipe], slot);
        return NULL;
    }
    return slot;
}

static void write_idle_reply(uint8_t pipe) {
    struct esb_payload payload = {0};
    payload.pipe = pipe;
    struct idle_reply *slot = fresh_idle_reply(pipe);
    if (slot != NULL) {
        payload.length = slot->length;
        memcpy(payload.data, slot->data, slot->length);
    }
    size_t pointer_offset = payload.length;
    bool has_pointer = append_pointer(pipe, &payload, false) != 0;
    if (payload.length == 0 || write_reply(&payload) != 0) {
        return;
    }
    if (slot != NULL) {
        (void)spsc_latch_release(&idle_reply_latch[pipe], slot);
    }
    commit_pointer(&payload, pointer_offset, has_pointer);
}

/* ISR-only, so esb_write_payload has a single caller context, no lock.
 * Shared ACK FIFO: a pipe's reply leaves only on that pipe's next packet asking for an ACK.
 * One write per such packet holds a pipe at two replies, a silent pipe keeps those two. */
void esb_link_role_rx_done(uint8_t pipes_acked) {
    for (uint8_t pipe = 0; pipe < REPLY_PIPE_COUNT; pipe++) {
        if ((pipes_acked & BIT(pipe)) == 0) {
            continue;
        }
        if (write_pending_control(pipe)) {
            continue;
        }
        if (write_queued_replies(pipe)) {
            continue;
        }
        write_idle_reply(pipe);
    }
}

void esb_link_role_tx_succeeded(void) {
}

bool esb_link_role_retry_failed_tx(void) {
    return false;
}

void esb_link_role_stop(void) {
}
