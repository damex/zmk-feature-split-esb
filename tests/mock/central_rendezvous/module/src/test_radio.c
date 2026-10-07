// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver and both halves under a real hop_central.c and esb_link_central.c.
 * Both halves go silent, then the right half returns camped and polls once per dip.
 * Exits 0 once dips land on anchors for one hop window, the silent walk covers the pool once,
 * and the right half reads the live epoch from the ACK of its one poll in a dip.
 * Exits 1 on a dip off the anchors or held too long, a wrong walk, a stale ACK, or at deadline.
 */
#define DT_DRV_COMPAT zmk_split_esb

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

#include <zmk_split_esb.h>

#include <esb.h>

#include "esb_keepalive.h"
#include "hop_internal.h"
#include "mock.h"
#include "mock_esb.h"

#define RIGHT_PIPE DT_PROP(DT_NODELABEL(right), pipe)
#define PIPE_COUNT DT_CHILD_NUM_STATUS_OKAY(DT_INST_CHILD(0, peripherals))
#define HOP_WINDOW_MS DT_INST_PROP(0, hop_window_ms)
#define START_MS 5
#define SAMPLE_MS 1
#define SILENCE_AT_MS 100
#define RETURN_AFTER_MS 1000
#define DIP_SLACK_MS 2
#define ACK_QUEUE_DEPTH 8
#define VERDICT_DEADLINE_MS 3000

enum phase {
    PHASE_POLLING,
    PHASE_SILENT,
    PHASE_CAMPED,
};

struct ack_queue {
    struct esb_payload slots[ACK_QUEUE_DEPTH];
    size_t head;
    size_t count;
    bool head_sent;
};

static const uint8_t anchor_channels[] = DT_INST_PROP(0, hop_anchors);

static enum phase phase = PHASE_POLLING;
static struct ack_queue ack_queues[PIPE_COUNT];
static uint8_t keepalive[ESB_KEEPALIVE_LENGTH(0, 0)];
static size_t keepalive_length;
static uint8_t epoch_at_silence;
static size_t dips;
static bool dip_open;
static uint32_t dip_start_ms;
static uint32_t camp_channel;

int esb_write_payload(const struct esb_payload *payload) {
    if (payload->pipe >= PIPE_COUNT) {
        printk("FAIL: ACK payload for pipe %u\n", payload->pipe);
        exit(1);
    }
    struct ack_queue *queue = &ack_queues[payload->pipe];
    if (queue->count == ACK_QUEUE_DEPTH) {
        return -ENOMEM;
    }
    queue->slots[(queue->head + queue->count) % ACK_QUEUE_DEPTH] = *payload;
    queue->count++;
    return 0;
}

/* NCS PRX: a new packet retires the payload the last ACK carried, the next head rides this ACK. */
static bool take_ack(uint8_t pipe, struct esb_payload *ack) {
    struct ack_queue *queue = &ack_queues[pipe];
    if (queue->head_sent && queue->count > 0) {
        queue->head = (queue->head + 1) % ACK_QUEUE_DEPTH;
        queue->count--;
    }
    queue->head_sent = queue->count > 0;
    if (queue->head_sent) {
        *ack = queue->slots[queue->head];
    }
    return queue->head_sent;
}

static bool poll_from(uint8_t pipe, struct esb_payload *ack) {
    bool carried = take_ack(pipe, ack);
    struct esb_payload poll = {.pipe = pipe, .length = (uint8_t)keepalive_length};
    memcpy(poll.data, keepalive, keepalive_length);
    mock_esb_rx_push(&poll);
    mock_esb_rx_raise();
    return carried;
}

static struct zmk_split_esb_status dongle_status(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    return status;
}

static bool is_anchor_channel(uint32_t channel) {
    return memchr(anchor_channels, (int)channel, sizeof(anchor_channels)) != NULL;
}

static void check_rejoin_ack(bool carried, const struct esb_payload *ack) {
    uint8_t live_epoch = dongle_status().epoch;
    mock_check((uint8_t)(live_epoch - epoch_at_silence) == HOP_COUNT,
               "silent walk stops after one pass over the pool");
    bool beacon = carried && esb_is_beacon(ack->data, ack->length);
    uint8_t beacon_epoch = beacon ? ack->data[offsetof(struct esb_beacon, epoch)] : 0;
    if (!beacon || beacon_epoch != live_epoch) {
        printk("right half ACK: %u bytes, beacon %d, epoch %u, live epoch %u\n",
               carried ? ack->length : 0, beacon, beacon_epoch, live_epoch);
    }
    mock_check(beacon && beacon_epoch == live_epoch,
               "camped half reads the live epoch from the ACK of its one poll in a dip");
    printk("PASS: all %u central rendezvous checks over %u dips\n",
           (unsigned int)mock_checks_passed(), (unsigned int)dips);
    exit(0);
}

static void camp_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (mock_esb_channel() != camp_channel) {
        return;
    }
    struct esb_payload ack = {0};
    bool carried = poll_from(RIGHT_PIPE, &ack);
    check_rejoin_ack(carried, &ack);
}
static K_WORK_DELAYABLE_DEFINE(camp_poll_work, camp_poll_fn);

static void open_dip(uint32_t channel, uint32_t now_ms) {
    dip_open = true;
    dip_start_ms = now_ms;
    dips++;
    mock_check(is_anchor_channel(channel), "dip lands on an anchor");
    if (phase == PHASE_CAMPED) {
        camp_channel = channel;
        k_work_reschedule(&camp_poll_work, K_MSEC(HOP_WINDOW_MS / 2));
    }
}

static void close_dip(uint32_t now_ms) {
    dip_open = false;
    mock_check(now_ms - dip_start_ms <= HOP_WINDOW_MS + DIP_SLACK_MS,
               "dip returns to the live channel after one hop window");
}

static void sampler_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sampler_work, sampler_fn);

static void sampler_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t radio_channel = mock_esb_channel();
    bool on_live = radio_channel == dongle_status().channel;
    uint32_t now_ms = k_uptime_get_32();
    if (!on_live && !dip_open) {
        open_dip(radio_channel, now_ms);
    } else if (on_live && dip_open) {
        close_dip(now_ms);
    }
    k_work_reschedule(&sampler_work, K_MSEC(SAMPLE_MS));
}

static void halves_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(halves_poll_work, halves_poll_fn);

static void halves_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (phase != PHASE_POLLING) {
        return;
    }
    if (mock_esb_channel() == dongle_status().channel) {
        struct esb_payload ack;
        for (uint8_t pipe = 0; pipe < PIPE_COUNT; pipe++) {
            (void)poll_from(pipe, &ack);
        }
    }
    k_work_reschedule(&halves_poll_work, K_MSEC(HOP_WINDOW_MS));
}

static void return_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(dips > 0, "dongle dips to anchors while the halves are lost");
    mock_check(dongle_status().epoch != epoch_at_silence,
               "dongle walks off the live channel while every half is silent");
    phase = PHASE_CAMPED;
}
static K_WORK_DELAYABLE_DEFINE(return_work, return_fn);

static void silence_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mock_check(dips == 0, "no dip while both halves poll");
    epoch_at_silence = dongle_status().epoch;
    phase = PHASE_SILENT;
    k_work_reschedule(&return_work, K_MSEC(RETURN_AFTER_MS));
}
static K_WORK_DELAYABLE_DEFINE(silence_work, silence_fn);

static void verdict_deadline_fn(struct k_work *work) {
    ARG_UNUSED(work);
    printk("FAIL: no camped poll landed in a dip, %u dips, phase %u\n", (unsigned int)dips,
           (unsigned int)phase);
    exit(1);
}
static K_WORK_DELAYABLE_DEFINE(verdict_deadline_work, verdict_deadline_fn);

static int test_radio_init(void) {
    static const uint8_t no_positions[ESB_KEEPALIVE_BITMAP_BYTES];
    const struct esb_keepalive_snapshot snapshot = {
        .state = ESB_KEEPALIVE_IDLE,
        .battery_level = ESB_KEEPALIVE_BATTERY_UNKNOWN,
        .position_bitmap = no_positions,
    };
    keepalive_length = esb_keepalive_encode(keepalive, sizeof(keepalive), &snapshot);
    k_work_reschedule(&halves_poll_work, K_MSEC(START_MS));
    k_work_reschedule(&sampler_work, K_MSEC(START_MS));
    k_work_reschedule(&silence_work, K_MSEC(SILENCE_AT_MS));
    k_work_reschedule(&verdict_deadline_work, K_MSEC(VERDICT_DEADLINE_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
