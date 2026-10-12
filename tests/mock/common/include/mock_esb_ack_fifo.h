// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake NCS ESB PRX ACK FIFO: CONFIG_ESB_TX_FIFO_SIZE slots shared by every pipe.
 * Provides esb_write_payload, a mock case linking it defines none.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esb.h>

/* Every call is a new packet, never a retransmit.
 * A packet asking for an ACK carries its pipe's queued head back as ACK payload.
 * That head leaves on the pipe's next packet asking for an ACK, raising TX success. */
void mock_esb_ack_fifo_receive(const struct esb_payload *payload);

size_t mock_esb_ack_fifo_full_refusals(void);
size_t mock_esb_ack_fifo_peak_slots(void);
size_t mock_esb_ack_fifo_queued(uint8_t pipe);
