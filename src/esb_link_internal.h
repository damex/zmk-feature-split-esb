// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Role seam between the shared radio layer (esb_link.c) and the per-role halves
 * (esb_link_central.c, esb_link_peripheral.c).
 */
#pragma once

#include <stdint.h>

#include <esb.h>

#if defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define ESB_LINK_ROLE_MODE ESB_MODE_PRX
#else
#define ESB_LINK_ROLE_MODE ESB_MODE_PTX
#endif

#define ESB_LINK_PIPE_MAX 8 /* ESB hardware pipe count */

struct esb_link_packet {
    uint8_t pipe;
    uint8_t length;
    uint8_t data[CONFIG_ZMK_SPLIT_ESB_MAX_PAYLOAD];
};

extern const uint8_t esb_link_pipe_count;

/* Role tail of radio setup and enable.
 * Central: starts RX.
 * Peripheral: no-op. */
int esb_link_role_start(void);

/* Radio ISR after RX FIFO drains, pipes_seen a bit per RXed pipe.
 * Central: drains those pipes' staged replies into ACK FIFO.
 * Peripheral: no-op. */
void esb_link_role_rx_done(uint8_t pipes_seen);

void esb_link_mark_tx_event(void);
uint32_t esb_link_tx_last_event_ms(void);

/* Held at most once. */
int esb_link_hfclk_acquire(void);
void esb_link_hfclk_release(void);
