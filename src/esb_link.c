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
#include <zephyr/sys/spsc_lockfree.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>

#include <esb.h>

#include "esb_link.h"

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(zmk_split_esb),
             "a zmk,split-esb node (base-address/prefix/hop-channels) is required");

static const uint8_t base_address[] = DT_INST_PROP(0, base_address);
static const uint8_t address_prefix = DT_INST_PROP(0, prefix);
static const int8_t tx_power_dbm = DT_INST_PROP(0, tx_power_dbm);
static const uint16_t retransmit_count = DT_INST_PROP(0, retransmit_count);
static const uint16_t retransmit_delay_us = DT_INST_PROP(0, retransmit_delay_us);
static const bool use_fast_ramp_up = DT_INST_PROP(0, use_fast_ramp_up);
static const uint8_t crc_bits = DT_INST_PROP(0, crc_bits);
static const uint16_t bitrate_kbps = DT_INST_PROP(0, bitrate_kbps);
BUILD_ASSERT(sizeof(base_address) == 4, "base-address must be exactly 4 bytes");

static const uint8_t hop_channels[] = DT_INST_PROP(0, hop_channels);
#define HOP_COUNT ARRAY_SIZE(hop_channels)
BUILD_ASSERT(HOP_COUNT >= 1, "hop-channels needs at least one channel");
static uint8_t hop_index;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static const uint16_t scan_dwell_ms = DT_INST_PROP(0, scan_dwell_ms);
/* Two keepalive periods, so one missed keepalive doesn't trigger a scan. */
#define LOCK_MISS_PERIODS 2
static const uint16_t active_lock_ms = LOCK_MISS_PERIODS * DT_INST_PROP(0, hop_window_ms);
static const uint16_t idle_lock_ms = LOCK_MISS_PERIODS * DT_INST_PROP(0, idle_keepalive_ms);
#else
static const uint16_t hop_threshold = DT_INST_PROP(0, hop_threshold);
static const uint16_t hop_window_ms = DT_INST_PROP(0, hop_window_ms);
static const uint16_t idle_keepalive_ms = DT_INST_PROP(0, idle_keepalive_ms);
static atomic_t fails_in_window;
static atomic_t data_sent_since_tick;
static uint8_t bad_windows; /* keepalive_work only */
#endif

/* Length-1 acked keepalive, never motion: a retransmit can't replay stale deltas.
 * Carries the hop signal under noack motion, holds the central lock when idle.
 * Its one byte is the peripheral's current rate. */
#define ESB_KEEPALIVE_LENGTH 1
#define KEEPALIVE_RATE_OFFSET 0
#define KEEPALIVE_RATE_IDLE 0x00
#define KEEPALIVE_RATE_ACTIVE 0x01

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

/* RX path: the radio ISR writes each payload straight into a lock-free SPSC ring
 * (no irq_lock, zero-copy slot) and signals the dispatch thread, which hands
 * packets to the role layer in thread context. Single producer (the ESB ISR),
 * single consumer (rx_thread), as the SPSC contract requires. */
SPSC_DEFINE(rx_spsc, struct esb_link_packet, CONFIG_ZMK_SPLIT_ESB_RX_QUEUE_SIZE);
static K_SEM_DEFINE(rx_sem, 0, 1);
static atomic_t rx_dropped;
static K_THREAD_STACK_DEFINE(rx_thread_stack, CONFIG_ZMK_SPLIT_ESB_RX_THREAD_STACK_SIZE);
static struct k_thread rx_thread;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Reverse-channel replies staged by a thread, drained into the ACK FIFO by the
 * ISR on the next received packet. */
K_MSGQ_DEFINE(reply_queue, sizeof(struct esb_link_packet), CONFIG_ZMK_SPLIT_ESB_REPLY_QUEUE_SIZE, 4);
#endif

static esb_link_rx_callback_t rx_callback;

/* Dedicated dispatch thread: drain the SPSC and hand each packet to the role
 * layer. Off the shared system workqueue so a high RX rate isn't gated by
 * unrelated work. */
static void rx_thread_fn(void *unused_a, void *unused_b, void *unused_c) {
    ARG_UNUSED(unused_a);
    ARG_UNUSED(unused_b);
    ARG_UNUSED(unused_c);
    while (1) {
        k_sem_take(&rx_sem, K_FOREVER);
        struct esb_link_packet *packet;
        while ((packet = spsc_consume(&rx_spsc)) != NULL) {
            if (rx_callback != NULL) {
                rx_callback(packet->data, packet->length);
            }
            spsc_release(&rx_spsc);
        }
    }
}

uint32_t esb_link_rx_dropped(void) {
    return (uint32_t)atomic_get(&rx_dropped);
}

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

/* Both ends hop the table in the same order, so they re-rendezvous within a sweep.
 * Leapfrog guard: HOP_COUNT * scan-dwell-ms < hop-threshold * hop-window-ms. */
static void apply_hop_channel(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    esb_stop_rx();
    esb_set_rf_channel(hop_channels[hop_index]);
    esb_start_rx();
#else
    esb_set_rf_channel(hop_channels[hop_index]); /* PTX: no RX to stop */
#endif
}

static void hop_next_channel(void) {
    hop_index = (uint8_t)((hop_index + 1U) % HOP_COUNT);
    apply_hop_channel();
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* On lock-timeout expiry the peer is lost: step a channel, re-arm at scan-dwell
 * to sweep until RX returns. */
static struct k_work_delayable scan_work;
static void scan_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    hop_next_channel();
    k_work_reschedule(&scan_work, K_MSEC(scan_dwell_ms));
}
#else
/* Hop after hop-threshold consecutive windows with a TX failure. */
static struct k_work_delayable keepalive_work;
static void keepalive_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (atomic_set(&fails_in_window, 0) > 0) {
        bad_windows++;
    } else {
        bad_windows = 0;
    }
    if (bad_windows >= hop_threshold) {
        bad_windows = 0;
        hop_next_channel();
    }
    bool active = atomic_set(&data_sent_since_tick, 0) != 0;
    struct esb_payload keepalive = {0};
    keepalive.pipe = 0;
    keepalive.length = ESB_KEEPALIVE_LENGTH;
    keepalive.data[KEEPALIVE_RATE_OFFSET] = active ? KEEPALIVE_RATE_ACTIVE : KEEPALIVE_RATE_IDLE;
    (void)esb_write_payload(&keepalive);
    k_work_reschedule(&keepalive_work, K_MSEC(active ? hop_window_ms : idle_keepalive_ms));
}
#endif

/* Runs in the ESB IRQ: only move bytes into queues, defer the ZMK handoff. */
static void on_esb_event(const struct esb_evt *event) {
    switch (event->evt_id) {
    case ESB_EVENT_RX_RECEIVED: {
        bool received = false;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        bool peer_active = false;
#endif
        struct esb_payload payload;
        while (esb_read_rx_payload(&payload) == 0) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
            if (payload.length == ESB_KEEPALIVE_LENGTH) {
                if (payload.data[KEEPALIVE_RATE_OFFSET] == KEEPALIVE_RATE_ACTIVE) {
                    peer_active = true;
                }
                continue; /* keepalive: never queued */
            }
            peer_active = true;
#endif
            struct esb_link_packet *slot = spsc_acquire(&rx_spsc);
            if (slot == NULL) {
                atomic_inc(&rx_dropped);
                continue; /* ring full; keep draining the radio FIFO */
            }
            slot->length = (uint8_t)MIN(payload.length, (int)sizeof(slot->data));
            memcpy(slot->data, payload.data, slot->length);
            spsc_produce(&rx_spsc);
            received = true;
        }
        if (received) {
            k_sem_give(&rx_sem);
        }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        stage_pending_replies();
        if (HOP_COUNT > 1) {
            k_work_reschedule(&scan_work, K_MSEC(peer_active ? active_lock_ms : idle_lock_ms));
        }
#endif
        break;
    }
    case ESB_EVENT_TX_FAILED: {
        /* Retransmits exhausted: drop the packet, flush so the TX FIFO can advance. */
        int flush_error = esb_flush_tx();
        if (flush_error) {
            LOG_DBG("esb_flush_tx after TX_FAILED returned %d", flush_error);
        }
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        if (HOP_COUNT > 1) {
            atomic_inc(&fails_in_window);
        }
#endif
        break;
    }
    default:
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
    set_error = esb_set_rf_channel(hop_channels[hop_index]);
    if (set_error) {
        LOG_DBG("esb_set_rf_channel returned %d", set_error);
    }
    set_error = esb_set_tx_power(tx_power_dbm);
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
    k_thread_create(&rx_thread, rx_thread_stack, K_THREAD_STACK_SIZEOF(rx_thread_stack),
                    rx_thread_fn, NULL, NULL, NULL, CONFIG_ZMK_SPLIT_ESB_RX_THREAD_PRIORITY, 0,
                    K_NO_WAIT);

    int error = hfclk_request();
    if (error) {
        LOG_ERR("HFCLK start failed (%d)", error);
        return error;
    }

    struct esb_config config = ESB_DEFAULT_CONFIG;
    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.mode = ESB_ROLE_MODE;
    config.event_handler = on_esb_event;
    config.bitrate = esb_bitrate_from_kbps(bitrate_kbps);
    config.crc = esb_crc_from_bits(crc_bits);
    config.retransmit_count = retransmit_count;
    config.retransmit_delay = retransmit_delay_us;
    /* Per-packet ACK is controlled by event_wants_ack() in peripheral.c; the
     * reverse channel rides the ACKs that the peripheral does request. */
    config.selective_auto_ack = true;
    config.tx_mode = ESB_TXMODE_AUTO;
    config.use_fast_ramp_up = use_fast_ramp_up;

    error = esb_init(&config);
    if (error) {
        LOG_ERR("esb_init failed (%d)", error);
        return error;
    }

    /* Init the hop work before radio_setup starts RX, or an early packet's ISR
     * reschedule would touch an uninitialized work. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (HOP_COUNT > 1) {
        k_work_init_delayable(&scan_work, scan_work_fn);
    }
#else
    if (HOP_COUNT > 1) {
        k_work_init_delayable(&keepalive_work, keepalive_work_fn);
    }
#endif

    error = esb_link_radio_setup();
    if (error) {
        return error;
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (HOP_COUNT > 1) {
        k_work_reschedule(&scan_work, K_MSEC(idle_lock_ms));
    }
#else
    if (HOP_COUNT > 1) {
        k_work_reschedule(&keepalive_work, K_MSEC(hop_window_ms));
    }
#endif
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    if (!enabled) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        if (HOP_COUNT > 1) {
            k_work_cancel_delayable(&scan_work);
        }
#else
        if (HOP_COUNT > 1) {
            k_work_cancel_delayable(&keepalive_work);
        }
#endif
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
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    int rx_error = esb_start_rx();
    if (HOP_COUNT > 1) {
        k_work_reschedule(&scan_work, K_MSEC(idle_lock_ms));
    }
    return rx_error;
#else
    if (HOP_COUNT > 1) {
        k_work_reschedule(&keepalive_work, K_MSEC(hop_window_ms));
    }
    return 0;
#endif
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
int esb_link_send(const uint8_t *data, size_t length, bool ack) {
    if (length > CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    if (HOP_COUNT > 1) {
        atomic_set(&data_sent_since_tick, 1); /* mark active so keepalive uses the fast rate */
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
