// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Log module registration, here because every role and transport compiles this file. */

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);
