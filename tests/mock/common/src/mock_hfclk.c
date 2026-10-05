// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HFXO stand-in for esb_link_hfclk.c, native_sim has no nRF clock control. */

#include "esb_link_internal.h"

int esb_link_hfclk_acquire(void) {
    return 0;
}

void esb_link_hfclk_release(void) {
}
