// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Single-device ESB radio layer.
 * One packet in, one out.
 * Reliability is ESB hardware ACK + retransmit, so no framing, reassembly,
 * software CRC or retry table here.
 * The reverse channel rides ACK payloads: the central stages a reply, it goes
 * out on the next received packet's ACK.
 * Channel hopping lives in hop.c.
 */
#define DT_DRV_COMPAT zmk_split_esb

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/spsc_lockfree.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(zmk_split_esb),
             "a zmk,split-esb node (base-address/peripherals/hop-channels) is required");

static const uint8_t base_address[] = DT_INST_PROP(0, base_address);
static const uint8_t address_length = DT_INST_PROP(0, address_length);
static const int8_t tx_power_dbm = DT_INST_PROP(0, tx_power_dbm);
static const uint16_t retransmit_count = DT_INST_PROP(0, retransmit_count);
static const uint16_t retransmit_delay_us = DT_INST_PROP(0, retransmit_delay_us);
static const bool use_fast_ramp_up = DT_INST_PROP(0, use_fast_ramp_up);
static const uint8_t crc_bits = DT_INST_PROP(0, crc_bits);
static const uint16_t bitrate_kbps = DT_INST_PROP(0, bitrate_kbps);
BUILD_ASSERT(sizeof(base_address) == 4, "base-address must be exactly 4 bytes");

#define ESB_PERIPHERALS DT_INST_CHILD(0, peripherals)
#define PERIPHERAL_PREFIX(node) [DT_PROP(node, pipe)] = DT_PROP(node, prefix),
static const uint8_t peripheral_prefixes[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, PERIPHERAL_PREFIX)
};
#define PERIPHERAL_COUNT ARRAY_SIZE(peripheral_prefixes)
BUILD_ASSERT(PERIPHERAL_COUNT >= 1, "peripherals needs at least one entry");
#define PIPE_BIT(node) +(1u << DT_PROP(node, pipe))
#define PIPE_BIT_SUM (0u DT_FOREACH_CHILD_STATUS_OKAY(ESB_PERIPHERALS, PIPE_BIT))
BUILD_ASSERT(PIPE_BIT_SUM == (1u << DT_CHILD_NUM_STATUS_OKAY(ESB_PERIPHERALS)) - 1u,
             "peripheral pipes must be exactly 0..N-1, one per peripheral");
const uint8_t esb_link_pipe_count = (uint8_t)PERIPHERAL_COUNT;

static enum esb_crc esb_crc_from_bits(uint8_t bits) {
    switch (bits) {
    case 0:
        return ESB_CRC_OFF;
    case 8:
        return ESB_CRC_8BIT;
    case 16:
        return ESB_CRC_16BIT;
    default:
        return ESB_CRC_16BIT;
    }
}

static enum esb_bitrate esb_bitrate_from_kbps(uint16_t kbps) {
    switch (kbps) {
    case 1000:
        return ESB_BITRATE_1MBPS;
    case 2000:
        return ESB_BITRATE_2MBPS;
    default:
        return ESB_BITRATE_2MBPS;
    }
}

/* NCS's CONFIG_ESB_MAX_PAYLOAD_LENGTH default (32) wins over ours on Kconfig parse order.
 * Raise it in your .conf or the radio rejects oversized payloads. */
BUILD_ASSERT(CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "set CONFIG_ESB_MAX_PAYLOAD_LENGTH >= CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD in your .conf");

/* RX path: the radio ISR writes each payload straight into a lock-free SPSC ring
 * (no irq_lock, zero-copy slot) and signals the dispatch thread, which hands packets
 * to the role layer in thread context.
 * Single producer (the ESB ISR), single consumer (rx_thread), as the SPSC contract
 * requires. */
SPSC_DEFINE(rx_spsc, struct esb_link_packet, CONFIG_ZMK_SPLIT_ESB_RX_QUEUE_SIZE);
static K_SEM_DEFINE(rx_sem, 0, 1);
static K_THREAD_STACK_DEFINE(rx_thread_stack, CONFIG_ZMK_SPLIT_ESB_RX_THREAD_STACK_SIZE);
static struct k_thread rx_thread;

static esb_link_rx_callback_t rx_callback;

static uint32_t tx_last_event_ms;

void esb_link_mark_tx_event(void) {
    tx_last_event_ms = k_uptime_get_32();
}

uint32_t esb_link_tx_last_event_ms(void) {
    return tx_last_event_ms;
}

/* Dedicated dispatch thread: drain the SPSC and hand each packet to the role layer.
 * Off the shared system workqueue so a high RX rate isn't gated by unrelated work. */
static FUNC_NORETURN void rx_thread_fn(void *unused_a, void *unused_b, void *unused_c) {
    ARG_UNUSED(unused_a);
    ARG_UNUSED(unused_b);
    ARG_UNUSED(unused_c);
    while (1) {
        k_sem_take(&rx_sem, K_FOREVER);
        struct esb_link_packet *packet;
        while ((packet = spsc_consume(&rx_spsc)) != NULL) {
            if (rx_callback != NULL) {
                rx_callback(packet->pipe, packet->data, packet->length);
            }
            spsc_release(&rx_spsc);
        }
    }
}

/* Runs in the ESB IRQ: only move bytes into queues, defer the ZMK handoff. */
static void on_esb_event(const struct esb_evt *event) {
    switch (event->evt_id) {
    case ESB_EVENT_RX_RECEIVED: {
        bool received = false;
        uint8_t pipes_seen = 0;
        struct esb_payload payload;
        while (esb_read_rx_payload(&payload) == 0) {
            pipes_seen |= (uint8_t)BIT(payload.pipe);
            if (hop_consume_rx(payload.pipe, payload.data, payload.length, payload.rssi)) {
                continue; /* control packet (keepalive or beacon), not queued */
            }
            struct esb_link_packet *slot = spsc_acquire(&rx_spsc);
            if (slot == NULL) {
                LOG_DBG("RX ring full, dropping packet");
                continue; /* keep draining the radio FIFO */
            }
            slot->pipe = payload.pipe;
            slot->length = (uint8_t)MIN(payload.length, (int)sizeof(slot->data));
            memcpy(slot->data, payload.data, slot->length);
            spsc_produce(&rx_spsc);
            received = true;
        }
        if (received) {
            k_sem_give(&rx_sem);
        }
        esb_link_role_rx_done(pipes_seen);
        break;
    }
    case ESB_EVENT_TX_SUCCESS:
        esb_link_mark_tx_event();
        hop_note_tx_success((uint8_t)event->tx_attempts);
        break;
    case ESB_EVENT_TX_FAILED: {
        esb_link_mark_tx_event();
        /* Retransmits exhausted: drop the packet, flush so the TX FIFO can advance. */
        LOG_WRN("TX retransmits exhausted, flushing TX FIFO");
        int flush_error = esb_flush_tx();
        if (flush_error) {
            LOG_DBG("esb_flush_tx after TX_FAILED returned %d", flush_error);
        }
        hop_note_tx_failed();
        break;
    }
    default:
        /* other ESB events carry nothing this layer acts on */
        break;
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

static K_MUTEX_DEFINE(hfclk_mutex);
static bool hfclk_held;

int esb_link_hfclk_acquire(void) {
    k_mutex_lock(&hfclk_mutex, K_FOREVER);
    int error = 0;
    if (!hfclk_held) {
        error = hfclk_request();
        if (error == 0) {
            hfclk_held = true;
        }
    }
    k_mutex_unlock(&hfclk_mutex);
    return error;
}

void esb_link_hfclk_release(void) {
    k_mutex_lock(&hfclk_mutex, K_FOREVER);
    if (hfclk_held) {
        struct onoff_manager *manager =
            z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
        int release_error = onoff_release(manager);
        if (release_error < 0) {
            LOG_DBG("onoff_release returned %d", release_error);
        }
        hfclk_held = false;
    }
    k_mutex_unlock(&hfclk_mutex);
}

/* Push base addresses, prefix, channel, TX power into the radio.
 * set_* failures are logged and ignored.
 * The radio starts on whatever values it already held. */
static int esb_link_radio_setup(void) {
    int set_error = esb_set_address_length(address_length);
    if (set_error) {
        LOG_DBG("esb_set_address_length returned %d", set_error);
    }
    set_error = esb_set_base_address_0(base_address);
    if (set_error) {
        LOG_DBG("esb_set_base_address_0 returned %d", set_error);
    }
    set_error = esb_set_base_address_1(base_address);
    if (set_error) {
        LOG_DBG("esb_set_base_address_1 returned %d", set_error);
    }
    set_error = esb_set_prefixes(peripheral_prefixes, (uint8_t)PERIPHERAL_COUNT);
    if (set_error) {
        LOG_DBG("esb_set_prefixes returned %d", set_error);
    }
    set_error = esb_set_rf_channel(hop_current_channel());
    if (set_error) {
        LOG_DBG("esb_set_rf_channel returned %d", set_error);
    }
    set_error = esb_set_tx_power(tx_power_dbm);
    if (set_error) {
        LOG_DBG("esb_set_tx_power returned %d", set_error);
    }
    return esb_link_role_start();
}

int esb_link_init(esb_link_rx_callback_t callback) {
    rx_callback = callback;
    k_thread_create(&rx_thread, rx_thread_stack, K_THREAD_STACK_SIZEOF(rx_thread_stack),
                    rx_thread_fn, NULL, NULL, NULL, CONFIG_ZMK_SPLIT_ESB_RX_THREAD_PRIORITY, 0,
                    K_NO_WAIT);

    int error = esb_link_hfclk_acquire();
    if (error) {
        LOG_ERR("HFCLK start failed (%d)", error);
        return error;
    }

    struct esb_config config = ESB_DEFAULT_CONFIG;
    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.mode = ESB_LINK_ROLE_MODE;
    config.event_handler = on_esb_event;
    config.bitrate = esb_bitrate_from_kbps(bitrate_kbps);
    config.crc = esb_crc_from_bits(crc_bits);
    config.retransmit_count = retransmit_count;
    config.retransmit_delay = retransmit_delay_us;
    /* Per-packet ACK is controlled by event_wants_ack() in peripheral.c.
     * The reverse channel rides the ACKs that the peripheral does request. */
    config.selective_auto_ack = true;
    config.tx_mode = ESB_TXMODE_AUTO;
    config.use_fast_ramp_up = use_fast_ramp_up;

    error = esb_init(&config);
    if (error) {
        LOG_ERR("esb_init failed (%d)", error);
        return error;
    }

    error = esb_link_radio_setup();
    if (error) {
        return error;
    }
    hop_start();
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    if (!enabled) {
        hop_stop();
        int stop_error = esb_stop_rx();
        if (stop_error) {
            LOG_DBG("esb_stop_rx returned %d", stop_error);
        }
        int flush_error = esb_flush_tx();
        if (flush_error) {
            LOG_DBG("esb_flush_tx returned %d", flush_error);
        }
        esb_link_hfclk_release();
        return 0;
    }
    int acquire_error = esb_link_hfclk_acquire();
    if (acquire_error) {
        LOG_ERR("HFCLK start failed (%d)", acquire_error);
        return acquire_error;
    }
    int start_error = esb_link_role_start();
    hop_start();
    return start_error;
}
