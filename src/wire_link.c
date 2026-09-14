// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Bidirectional UART link between two split peripherals.
 * Framing lives in wire_frame.c.
 * UART node from chosen zmk,esb-wire.
 * Physical UART or USB CDC.
 */

#include "wire_link.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "wire_frame.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define WIRE_LINK_UART_NODE          DT_CHOSEN(zmk_esb_wire)
BUILD_ASSERT(DT_NODE_EXISTS(WIRE_LINK_UART_NODE),
             "chosen zmk,esb-wire must reference a UART node");

#define WIRE_LINK_RX_RING_BYTES      (WIRE_FRAME_MAX_ENCODED * 2)
#define WIRE_LINK_CHUNK_BYTES        64
#define WIRE_LINK_TX_LOCK_TIMEOUT_MS 50

static const struct device *const wire_uart_device = DEVICE_DT_GET(WIRE_LINK_UART_NODE);

RING_BUF_DECLARE(wire_rx_ring, WIRE_LINK_RX_RING_BYTES);

static struct wire_frame_parser wire_parser;

static atomic_ptr_t wire_rx_subscription;

static atomic_t wire_peer_last_rx_uptime;
static struct k_work_delayable wire_keepalive_work;

static uint8_t wire_tx_framed[WIRE_FRAME_MAX_ENCODED];
static K_SEM_DEFINE(wire_tx_lock, 1, 1);

static K_THREAD_STACK_DEFINE(wire_rx_stack, CONFIG_ZMK_SPLIT_ESB_WIRE_RX_STACK_SIZE);
static struct k_thread wire_rx_thread_data;
static K_SEM_DEFINE(wire_rx_wake, 0, 1);

static void wire_on_frame(const uint8_t *payload, size_t length, void *user_data) {
    ARG_UNUSED(user_data);
    atomic_set(&wire_peer_last_rx_uptime, (atomic_val_t)k_uptime_get_32());
    if (length == 0) {
        return;
    }
    struct wire_link_subscription *subscription = atomic_ptr_get(&wire_rx_subscription);
    if (subscription != NULL && subscription->callback != NULL) {
        subscription->callback(payload, length, subscription->user_data);
    }
}

static void wire_process_ring(void) {
    uint8_t chunk[WIRE_LINK_CHUNK_BYTES];
    while (true) {
        const uint32_t read = ring_buf_get(&wire_rx_ring, chunk, sizeof(chunk));
        if (read == 0) {
            return;
        }
        wire_frame_parser_ingest(&wire_parser, chunk, read, wire_on_frame, NULL);
    }
}

static void wire_rx_thread_entry(void *unused_a, void *unused_b, void *unused_c) {
    ARG_UNUSED(unused_a);
    ARG_UNUSED(unused_b);
    ARG_UNUSED(unused_c);
    while (true) {
        k_sem_take(&wire_rx_wake, K_FOREVER);
        wire_process_ring();
    }
}

static void wire_uart_isr(const struct device *uart_device, void *user_data) {
    ARG_UNUSED(user_data);
    if (uart_irq_update(uart_device) <= 0) {
        return;
    }
    while (uart_irq_rx_ready(uart_device) > 0) {
        uint8_t buffer[WIRE_LINK_CHUNK_BYTES];
        const int read = uart_fifo_read(uart_device, buffer, sizeof(buffer));
        if (read <= 0) {
            break;
        }
        const uint32_t written = ring_buf_put(&wire_rx_ring, buffer, (uint32_t)read);
        if (written < (uint32_t)read) {
            LOG_WRN("wire rx ring overrun, %d bytes dropped", read - (int)written);
        }
    }
    k_sem_give(&wire_rx_wake);
}

int wire_link_register_rx(struct wire_link_subscription *subscription) {
    atomic_ptr_set(&wire_rx_subscription, subscription);
    return 0;
}

int wire_link_send(const uint8_t *payload, size_t length) {
    if (length > WIRE_FRAME_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    if (k_sem_take(&wire_tx_lock, K_MSEC(WIRE_LINK_TX_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }
    const int encoded = wire_frame_encode(payload, length,
                                          wire_tx_framed, sizeof(wire_tx_framed));
    if (encoded < 0) {
        k_sem_give(&wire_tx_lock);
        return encoded;
    }
    for (int byte_index = 0; byte_index < encoded; byte_index++) {
        uart_poll_out(wire_uart_device, wire_tx_framed[byte_index]);
    }
    k_sem_give(&wire_tx_lock);
    return 0;
}

bool wire_link_is_up(void) {
    const uint32_t last_rx = (uint32_t)atomic_get(&wire_peer_last_rx_uptime);
    if (last_rx == 0) {
        return false;
    }
    const uint32_t elapsed = k_uptime_get_32() - last_rx;
    return elapsed <= CONFIG_ZMK_SPLIT_ESB_WIRE_TIMEOUT_MS;
}

static void wire_keepalive_fire(struct k_work *work) {
    ARG_UNUSED(work);
    const int send_result = wire_link_send(NULL, 0);
    if (send_result != 0 && send_result != -EBUSY) {
        LOG_WRN("wire keepalive send failed (%d)", send_result);
    }
    k_work_reschedule(&wire_keepalive_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_WIRE_KEEPALIVE_MS));
}

static int wire_link_init(void) {
    if (!device_is_ready(wire_uart_device)) {
        LOG_ERR("wire uart device not ready");
        return -ENODEV;
    }
    uart_irq_rx_disable(wire_uart_device);
    uart_irq_tx_disable(wire_uart_device);
    uart_irq_callback_set(wire_uart_device, wire_uart_isr);
    uart_irq_rx_enable(wire_uart_device);
    k_thread_create(&wire_rx_thread_data, wire_rx_stack,
                    K_THREAD_STACK_SIZEOF(wire_rx_stack),
                    wire_rx_thread_entry, NULL, NULL, NULL,
                    CONFIG_ZMK_SPLIT_ESB_WIRE_RX_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&wire_rx_thread_data, "wire_link_rx");
    k_work_init_delayable(&wire_keepalive_work, wire_keepalive_fire);
    k_work_reschedule(&wire_keepalive_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_WIRE_KEEPALIVE_MS));
    LOG_INF("wire link up on %s", wire_uart_device->name);
    return 0;
}

SYS_INIT(wire_link_init, APPLICATION, CONFIG_ZMK_SPLIT_ESB_PRIORITY);
