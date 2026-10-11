// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* HID relay sink registration. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called once per changed report, one whole report each.
 * Return 0 once delivered, nonzero leaves it for the next re-send to retry.
 * Skippable: refuse rather than wait, the next report carries everything in this one. */
typedef int (*zmk_split_esb_hid_relay_callback_t)(const uint8_t *bytes, size_t length,
                                                   bool skippable);

int zmk_split_esb_hid_relay_register(zmk_split_esb_hid_relay_callback_t callback);
