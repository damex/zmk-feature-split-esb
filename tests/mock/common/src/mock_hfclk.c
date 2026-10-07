// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Fake esb_link_hfclk.c, native_sim has no nRF clock control. */

#include "mock_hfclk.h"

#include "esb_link_internal.h"

static bool held;

int esb_link_hfclk_acquire(void) {
    held = true;
    return 0;
}

void esb_link_hfclk_release(void) {
    held = false;
}

bool mock_hfclk_held(void) {
    return held;
}
