// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Fake esb_link_hfclk.c, native_sim has no nRF clock control. */
#pragma once

#include <stdbool.h>

bool mock_hfclk_held(void);
