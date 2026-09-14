// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*zmk_split_esb_hid_relay_callback_t)(const uint8_t *bytes, size_t length);

int zmk_split_esb_hid_relay_register(zmk_split_esb_hid_relay_callback_t callback);
