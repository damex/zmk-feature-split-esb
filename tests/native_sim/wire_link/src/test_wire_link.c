// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>

#include "wire_frame.h"
#include "wire_link.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define WIRE_UART DEVICE_DT_GET(DT_CHOSEN(zmk_esb_wire))
#define CAPTURE_BYTES 1024
#define PUMP_ROUNDS_MAX 2048
#define FRAMES_MAX 8
#define RX_WAIT_MS 100
#define RX_SETTLE_MS 10

struct frame_log {
    size_t count;
    size_t lengths[FRAMES_MAX];
    uint8_t payloads[FRAMES_MAX][WIRE_FRAME_MAX_PAYLOAD];
};

static void record_frame(const uint8_t *payload, size_t length, void *user_data) {
    struct frame_log *log = user_data;
    if (log->count >= FRAMES_MAX) {
        return;
    }
    log->lengths[log->count] = length;
    memcpy(log->payloads[log->count], payload, length);
    log->count++;
}

static size_t pump_tx(uint8_t *out, size_t out_size) {
    size_t total = 0;
    for (size_t round = 0; round < PUMP_ROUNDS_MAX && total < out_size; round++) {
        k_sleep(K_MSEC(1));
        uint32_t read = uart_emul_get_tx_data(WIRE_UART, &out[total], out_size - total);
        if (read == 0) {
            break;
        }
        total += read;
        uart_irq_tx_enable(WIRE_UART);
    }
    return total;
}

static void decode(const uint8_t *bytes, size_t length, struct frame_log *log) {
    struct wire_frame_parser parser = {0};
    wire_frame_parser_ingest(&parser, bytes, length, record_frame, log);
}

static struct frame_log rx_log;
static K_SEM_DEFINE(rx_sem, 0, FRAMES_MAX);

static void on_rx(const uint8_t *payload, size_t length, void *user_data) {
    record_frame(payload, length, user_data);
    k_sem_give(&rx_sem);
}

static struct wire_link_subscription rx_subscription = {
    .callback = on_rx,
    .user_data = &rx_log,
};

static void *suite_setup(void) {
    zassert_ok(wire_link_register_rx(&rx_subscription));
    return NULL;
}

static void before_each(void *fixture) {
    ARG_UNUSED(fixture);
    uint8_t scratch[CAPTURE_BYTES];
    (void)pump_tx(scratch, sizeof(scratch));
    memset(&rx_log, 0, sizeof(rx_log));
    k_sem_reset(&rx_sem);
}

ZTEST_SUITE(wire_link, NULL, suite_setup, before_each, NULL, NULL);

ZTEST(wire_link, test_event_frame_reaches_wire) {
    const uint8_t event[] = {0xB1, 0x00, 0xB3};
    zassert_ok(wire_link_send_event(event, sizeof(event)));
    uint8_t wire[CAPTURE_BYTES];
    struct frame_log log = {0};
    decode(wire, pump_tx(wire, sizeof(wire)), &log);
    zassert_equal(log.count, 1, "one frame on the wire");
    zassert_equal(log.lengths[0], sizeof(event), "event length kept");
    zassert_mem_equal(log.payloads[0], event, sizeof(event), "event bytes kept");
}

ZTEST(wire_link, test_keepalive_and_event_frames_stay_whole) {
    const uint8_t keepalive[] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA};
    const uint8_t event[] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA};
    zassert_ok(wire_link_send_keepalive(keepalive, sizeof(keepalive)));
    k_sleep(K_MSEC(1));
    zassert_ok(wire_link_send_event(event, sizeof(event)));
    uint8_t wire[CAPTURE_BYTES];
    struct frame_log log = {0};
    decode(wire, pump_tx(wire, sizeof(wire)), &log);
    zassert_equal(log.count, 2, "both frames decode whole");
    zassert_mem_equal(log.payloads[0], keepalive, sizeof(keepalive), "keepalive first");
    zassert_mem_equal(log.payloads[1], event, sizeof(event), "event second");
}

ZTEST(wire_link, test_rx_frame_reaches_subscriber) {
    const uint8_t junk[] = {0x55, 0x55, 0x00};
    const uint8_t payload[] = {0xC1, 0x00, 0xC3};
    uint8_t frame[WIRE_FRAME_MAX_ENCODED];
    int length = wire_frame_encode(payload, sizeof(payload), frame, sizeof(frame));
    zassert_true(length > 0, "frame encodes");
    zassert_equal(uart_emul_put_rx_data(WIRE_UART, junk, sizeof(junk)), sizeof(junk));
    zassert_equal(uart_emul_put_rx_data(WIRE_UART, frame, (size_t)length), (uint32_t)length);
    zassert_ok(k_sem_take(&rx_sem, K_MSEC(RX_WAIT_MS)), "frame delivered");
    zassert_equal(rx_log.count, 1, "junk skipped, one frame delivered");
    zassert_equal(rx_log.lengths[0], sizeof(payload), "payload length kept");
    zassert_mem_equal(rx_log.payloads[0], payload, sizeof(payload), "payload bytes kept");
}

ZTEST(wire_link, test_link_up_follows_rx) {
    uint8_t frame[WIRE_FRAME_MAX_ENCODED];
    int length = wire_frame_encode(NULL, 0, frame, sizeof(frame));
    zassert_true(length > 0, "keepalive frame encodes");
    zassert_equal(uart_emul_put_rx_data(WIRE_UART, frame, (size_t)length), (uint32_t)length);
    k_sleep(K_MSEC(RX_SETTLE_MS));
    zassert_true(wire_link_is_up(), "rx marks link up");
    k_sleep(K_MSEC(CONFIG_ZMK_SPLIT_ESB_WIRE_TIMEOUT_MS + 1));
    zassert_false(wire_link_is_up(), "silence past timeout marks link down");
}
