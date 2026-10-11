// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB driver under a real esb_link.c and esb_link_central.c, hop faked out.
 * Exits 0 once every RX gets at most one ACK write, control first, then queued replies,
 * then a fresh idle reply, only on the pipe that received, and a failed write retries.
 * Exits 1 on a wrong, missing or extra ACK write.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"
#include "mock.h"
#include "mock_esb.h"

#define LEFT_PIPE DT_PROP(DT_NODELABEL(left), pipe)
#define RIGHT_PIPE DT_PROP(DT_NODELABEL(right), pipe)
#define SCRIPT_DELAY_MS 20
#define RX_TAG 0x01
#define BEACON_TAG 0xB1
#define MASK_TAG 0xC1
#define REPLY_TAG 0xD1
#define IDLE_TAG 0xE1
#define LATE_REPLY_TAG 0xD2
#define STALE_IDLE_TAG 0xE2
#define RIGHT_REPLY_TAG 0xD3
#define OLD_BEACON_TAG 0xB2
#define NEW_BEACON_TAG 0xB3
#define RETRIED_BEACON_TAG 0xB4

static size_t writes;
static uint8_t last_pipe;
static uint8_t last_tag;
static bool fail_next_write;

void hop_start(void) {
}

void hop_stop(void) {
}

bool hop_consume_rx(uint8_t pipe, const uint8_t *data, uint8_t length, int8_t rssi) {
    ARG_UNUSED(pipe);
    ARG_UNUSED(data);
    ARG_UNUSED(length);
    ARG_UNUSED(rssi);
    return false;
}

void hop_note_tx_success(uint8_t attempts) {
    ARG_UNUSED(attempts);
}

void hop_note_tx_failed(void) {
}

uint8_t hop_current_channel(void) {
    return 0;
}

int esb_write_payload(const struct esb_payload *payload) {
    writes++;
    last_pipe = payload->pipe;
    last_tag = payload->data[0];
    if (fail_next_write) {
        fail_next_write = false;
        return -ENOMEM;
    }
    return 0;
}

static void on_rx(uint8_t pipe, const uint8_t *data, size_t length) {
    ARG_UNUSED(pipe);
    ARG_UNUSED(data);
    ARG_UNUSED(length);
}

static void receive_on(uint8_t pipe) {
    struct esb_payload payload = {.pipe = pipe, .length = 2, .data = {RX_TAG, RX_TAG}};
    mock_esb_rx_push(&payload);
    mock_esb_rx_raise();
}

static void expect_reply(uint8_t pipe, uint8_t tag, const char *what) {
    size_t writes_before = writes;
    receive_on(pipe);
    bool replied = writes == writes_before + 1 && last_pipe == pipe && last_tag == tag;
    if (!replied) {
        printk("%u writes, last pipe %u tag 0x%02x, expected pipe %u tag 0x%02x\n",
               (unsigned int)(writes - writes_before), last_pipe, last_tag, pipe, tag);
    }
    mock_check(replied, what);
}

static void expect_no_reply(uint8_t pipe, const char *what) {
    size_t writes_before = writes;
    receive_on(pipe);
    mock_check(writes == writes_before, what);
}

static void stage_reply(uint8_t pipe, uint8_t tag) {
    const uint8_t data[] = {tag, tag};
    mock_check(esb_link_stage_reply(pipe, data, sizeof(data)) == 0, "reply staged");
}

static void latch_control(uint8_t pipe, enum esb_link_control kind, uint8_t tag) {
    const uint8_t data[] = {tag, tag};
    mock_check(esb_link_latch_control(pipe, kind, data, sizeof(data)) == 0, "control latched");
}

static void latch_idle(uint8_t pipe, uint8_t tag) {
    const uint8_t data[] = {tag, tag};
    mock_check(esb_link_latch_idle_reply(pipe, data, sizeof(data)) == 0, "idle reply latched");
}

static void check_priority(void) {
    stage_reply(LEFT_PIPE, REPLY_TAG);
    latch_idle(LEFT_PIPE, IDLE_TAG);
    latch_control(LEFT_PIPE, ESB_LINK_CONTROL_MASK, MASK_TAG);
    latch_control(LEFT_PIPE, ESB_LINK_CONTROL_BEACON, BEACON_TAG);
    expect_reply(LEFT_PIPE, BEACON_TAG, "beacon goes first");
    expect_reply(LEFT_PIPE, MASK_TAG, "mask update after the beacon");
    expect_reply(LEFT_PIPE, REPLY_TAG, "queued reply after control");
    expect_reply(LEFT_PIPE, IDLE_TAG, "idle reply once nothing else waits");
    expect_no_reply(LEFT_PIPE, "idle reply goes out once");
}

static void check_stale_idle(void) {
    latch_idle(LEFT_PIPE, STALE_IDLE_TAG);
    stage_reply(LEFT_PIPE, LATE_REPLY_TAG);
    expect_reply(LEFT_PIPE, LATE_REPLY_TAG, "reply staged after the idle reply goes out");
    expect_no_reply(LEFT_PIPE, "idle reply older than a reply is dropped");
}

static void check_received_pipe_only(void) {
    stage_reply(RIGHT_PIPE, RIGHT_REPLY_TAG);
    expect_no_reply(LEFT_PIPE, "reply waits for its own pipe to receive");
    expect_reply(RIGHT_PIPE, RIGHT_REPLY_TAG, "reply goes out when its pipe receives");
}

static void check_latest_control_and_retry(void) {
    latch_control(LEFT_PIPE, ESB_LINK_CONTROL_BEACON, OLD_BEACON_TAG);
    latch_control(LEFT_PIPE, ESB_LINK_CONTROL_BEACON, NEW_BEACON_TAG);
    expect_reply(LEFT_PIPE, NEW_BEACON_TAG, "newer beacon replaces the older one");
    expect_no_reply(LEFT_PIPE, "replaced beacon never goes out");
    latch_control(LEFT_PIPE, ESB_LINK_CONTROL_BEACON, RETRIED_BEACON_TAG);
    fail_next_write = true;
    expect_reply(LEFT_PIPE, RETRIED_BEACON_TAG, "beacon write attempted");
    expect_reply(LEFT_PIPE, RETRIED_BEACON_TAG, "failed beacon write retries on the next RX");
    expect_no_reply(LEFT_PIPE, "retried beacon released once written");
}

static void script_fn(struct k_work *work) {
    ARG_UNUSED(work);
    check_priority();
    check_stale_idle();
    check_received_pipe_only();
    check_latest_control_and_retry();
    printk("PASS: all %u return path checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(script_work, script_fn);

static int test_radio_init(void) {
    mock_check(esb_link_init(on_rx) == 0, "link starts");
    k_work_reschedule(&script_work, K_MSEC(SCRIPT_DELAY_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
