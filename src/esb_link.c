// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Single-device ESB radio layer. One packet in, one out; reliability is ESB
 * hardware ACK + retransmit, so no framing, reassembly, software CRC or retry
 * table here. The reverse channel rides ACK payloads: the central stages a reply,
 * it goes out on the next received packet's ACK.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>

#include <esb.h>

#include "esb_link.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(zmk_split_esb),
             "a zmk,split-esb node (base-address/prefix/rf-channel) is required");

static const uint8_t base_address[] = DT_INST_PROP(0, base_address);
static const uint8_t address_prefix = DT_INST_PROP(0, prefix);
static const uint8_t rf_channel = DT_INST_PROP(0, rf_channel);
BUILD_ASSERT(sizeof(base_address) == 4, "base-address must be exactly 4 bytes");

/* NCS's CONFIG_ESB_MAX_PAYLOAD_LENGTH default (32) wins over ours on Kconfig parse
 * order; raise it in your .conf or the radio rejects oversized payloads. */
BUILD_ASSERT(CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "set CONFIG_ESB_MAX_PAYLOAD_LENGTH >= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD in your .conf");

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define ESB_ROLE_MODE ESB_MODE_PRX
#else
#define ESB_ROLE_MODE ESB_MODE_PTX
#endif

struct esb_link_packet {
    uint8_t length;
    uint8_t data[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
};

/* RX path: the radio ISR copies a payload here; a work item hands it to the role
 * layer in thread context. */
K_MSGQ_DEFINE(rx_queue, sizeof(struct esb_link_packet), CONFIG_ZMK_SPLIT_ESB_RX_QUEUE_SIZE, 4);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Reverse-channel replies staged by a thread, drained into the ACK FIFO by the
 * ISR on the next received packet. */
K_MSGQ_DEFINE(reply_queue, sizeof(struct esb_link_packet), CONFIG_ZMK_SPLIT_ESB_REPLY_QUEUE_SIZE, 4);
#endif

static esb_link_rx_callback_t rx_callback;

static void rx_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    struct esb_link_packet packet;
    while (k_msgq_get(&rx_queue, &packet, K_NO_WAIT) == 0) {
        if (rx_callback != NULL) {
            rx_callback(packet.data, packet.length);
        }
    }
}
static K_WORK_DEFINE(rx_work, rx_work_handler);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Drain staged replies into the ACK FIFO so each rides the next ACK back to the
 * peripheral. Peek-then-remove keeps a reply queued if the ACK FIFO is full.
 * ISR-only, so esb_write_payload has a single caller context (no lock needed). */
static void stage_pending_replies(void) {
    struct esb_link_packet packet;
    while (k_msgq_peek(&reply_queue, &packet) == 0) {
        struct esb_payload payload = {0};
        payload.pipe = 0;
        payload.length = packet.length;
        memcpy(payload.data, packet.data, packet.length);
        if (esb_write_payload(&payload) != 0) {
            break; /* ACK FIFO full; retry on the next received packet */
        }
        (void)k_msgq_get(&reply_queue, &packet, K_NO_WAIT);
    }
}
#endif

/* Runs in the ESB IRQ: only move bytes into queues, defer the ZMK handoff. */
static void on_esb_event(const struct esb_evt *event) {
    switch (event->evt_id) {
    case ESB_EVENT_RX_RECEIVED: {
        bool received = false;
        struct esb_payload payload;
        while (esb_read_rx_payload(&payload) == 0) {
            struct esb_link_packet packet;
            packet.length = (uint8_t)MIN(payload.length, (int)sizeof(packet.data));
            memcpy(packet.data, payload.data, packet.length);
            if (k_msgq_put(&rx_queue, &packet, K_NO_WAIT) != 0) {
                LOG_WRN("RX queue full, dropping packet");
            }
            received = true;
        }
        if (received) {
            k_work_submit(&rx_work);
        }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        stage_pending_replies();
#endif
        break;
    }
    case ESB_EVENT_TX_FAILED: {
        /* Retransmits exhausted: drop the packet, flush so the TX FIFO can advance. */
        int flush_error = esb_flush_tx();
        if (flush_error) {
            LOG_DBG("esb_flush_tx after TX_FAILED returned %d", flush_error);
        }
        break;
    }
    default:
        break; /* TX_SUCCESS and any other event: nothing to do */
    }
}

static int hfclk_request(void) {
    struct onoff_manager *manager = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    struct onoff_client client;
    sys_notify_init_spinwait(&client.notify);
    int error = onoff_request(manager, &client);
    if (error < 0) {
        return error;
    }
    int result;
    while (sys_notify_fetch_result(&client.notify, &result) == -EAGAIN) {
    }
    return result;
}

/* Push base addresses, prefix, channel, TX power into the radio.
 * set_* failures are logged and ignored; the radio starts on whatever
 * values it already held. */
static int esb_link_radio_setup(void) {
    uint8_t base_address_1[4] = {0};
    uint8_t prefixes[1] = {address_prefix};
    int set_error = esb_set_base_address_0(base_address);
    if (set_error) {
        LOG_DBG("esb_set_base_address_0 returned %d", set_error);
    }
    set_error = esb_set_base_address_1(base_address_1);
    if (set_error) {
        LOG_DBG("esb_set_base_address_1 returned %d", set_error);
    }
    set_error = esb_set_prefixes(prefixes, ARRAY_SIZE(prefixes));
    if (set_error) {
        LOG_DBG("esb_set_prefixes returned %d", set_error);
    }
    set_error = esb_set_rf_channel(rf_channel);
    if (set_error) {
        LOG_DBG("esb_set_rf_channel returned %d", set_error);
    }
    set_error = esb_set_tx_power(CONFIG_ZMK_SPLIT_ESB_TX_POWER_DBM);
    if (set_error) {
        LOG_DBG("esb_set_tx_power returned %d", set_error);
    }
    if (ESB_ROLE_MODE == ESB_MODE_PRX) {
        return esb_start_rx();
    }
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    rx_callback = callback;

    int error = hfclk_request();
    if (error) {
        LOG_ERR("HFCLK start failed (%d)", error);
        return error;
    }

    struct esb_config config = ESB_DEFAULT_CONFIG;
    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.mode = ESB_ROLE_MODE;
    config.event_handler = on_esb_event;
    config.bitrate =
        IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_BITRATE_2MBPS) ? ESB_BITRATE_2MBPS : ESB_BITRATE_1MBPS;
    config.crc = ESB_CRC_16BIT;
    config.retransmit_count = CONFIG_ZMK_SPLIT_ESB_RETRANSMIT_COUNT;
    config.retransmit_delay = CONFIG_ZMK_SPLIT_ESB_RETRANSMIT_DELAY_US;
    /* Per-packet ACK is controlled by event_wants_ack() in peripheral.c; the
     * reverse channel rides the ACKs that the peripheral does request. */
    config.selective_auto_ack = true;
    config.tx_mode = ESB_TXMODE_AUTO;

    error = esb_init(&config);
    if (error) {
        LOG_ERR("esb_init failed (%d)", error);
        return error;
    }

    return esb_link_radio_setup();
}

int esb_link_set_enabled(bool enabled) {
    if (!enabled) {
        int stop_error = esb_stop_rx();
        if (stop_error) {
            LOG_DBG("esb_stop_rx returned %d", stop_error);
        }
        int flush_error = esb_flush_tx();
        if (flush_error) {
            LOG_DBG("esb_flush_tx returned %d", flush_error);
        }
        return 0;
    }
    return (ESB_ROLE_MODE == ESB_MODE_PRX) ? esb_start_rx() : 0;
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    if (length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    struct esb_payload payload = {0};
    payload.pipe = 0;
    payload.noack = !ack;
    payload.length = (uint8_t)length;
    memcpy(payload.data, data, length);
    return esb_write_payload(&payload);
}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
int esb_link_stage_reply(const uint8_t *data, size_t length) {
    if (length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    struct esb_link_packet packet;
    packet.length = (uint8_t)length;
    memcpy(packet.data, data, length);
    if (k_msgq_put(&reply_queue, &packet, K_NO_WAIT) != 0) {
        return -ENOBUFS;
    }
    return 0;
}
#endif
