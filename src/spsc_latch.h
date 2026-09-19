// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>

struct spsc_latch {
    atomic_ptr_t published;
    atomic_t next_index;
    uint8_t slot_count;
};

uint8_t spsc_latch_claim(struct spsc_latch *latch);

void spsc_latch_publish(struct spsc_latch *latch, void *slot);

void *spsc_latch_peek(const struct spsc_latch *latch);

/* Nulls only when still matching, so a publish between peek and release is not dropped. */
bool spsc_latch_release(struct spsc_latch *latch, void *expected);
