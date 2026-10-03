// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Single-producer latest-value latch. */

#include "spsc_latch.h"

#include <zephyr/sys/__assert.h>

uint8_t spsc_latch_claim(struct spsc_latch *latch) {
    __ASSERT_NO_MSG(latch != NULL);
    __ASSERT_NO_MSG(latch->slot_count >= 2);
    atomic_val_t index = atomic_inc(&latch->next_index);
    return (uint8_t)((uint32_t)index % latch->slot_count);
}

void spsc_latch_publish(struct spsc_latch *latch, void *slot) {
    __ASSERT_NO_MSG(latch != NULL);
    atomic_ptr_set(&latch->published, slot);
}

void *spsc_latch_peek(const struct spsc_latch *latch) {
    __ASSERT_NO_MSG(latch != NULL);
    return atomic_ptr_get(&latch->published);
}

bool spsc_latch_release(struct spsc_latch *latch, void *expected) {
    __ASSERT_NO_MSG(latch != NULL);
    return atomic_ptr_cas(&latch->published, expected, NULL);
}
