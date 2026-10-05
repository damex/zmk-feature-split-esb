// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Verdict helpers shared by mock cases. */

#include "mock.h"

#include <stdlib.h>

#include <zephyr/sys/printk.h>

static size_t checks_passed;

void mock_print_bytes(const uint8_t *data, size_t length) {
    for (size_t index = 0; index < length; index++) {
        printk(" %02x", data[index]);
    }
    printk("\n");
}

void mock_check(bool passed, const char *what) {
    if (!passed) {
        printk("FAIL: %s\n", what);
        exit(1);
    }
    checks_passed++;
}

size_t mock_checks_passed(void) {
    return checks_passed;
}
