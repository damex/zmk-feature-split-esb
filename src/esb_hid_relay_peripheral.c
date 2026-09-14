// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <zephyr/kernel.h>

#include <zmk_split_esb_hid_relay.h>

#include "esb_hid_relay_peripheral.h"

static atomic_ptr_t hid_relay_callback;

int zmk_split_esb_hid_relay_register(zmk_split_esb_hid_relay_callback_t callback) {
    atomic_ptr_set(&hid_relay_callback, callback);
    return 0;
}

void esb_hid_relay_deliver(const uint8_t *bytes, size_t length) {
    zmk_split_esb_hid_relay_callback_t callback = atomic_ptr_get(&hid_relay_callback);
    if (callback != NULL) {
        callback(bytes, length);
    }
}
