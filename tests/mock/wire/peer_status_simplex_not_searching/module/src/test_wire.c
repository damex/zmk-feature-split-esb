// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Test tap on a simplex wire peer's UART.
 * Exits 0 once the wire peer status reads not searching on a wire it cannot receive on.
 * Exits 1 on a searching status.
 */
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/sys/printk.h>

#include <zmk_split_esb.h>

#include "mock.h"

static int test_wire_init(void) {
    struct zmk_split_esb_status status;
    zmk_split_esb_get_status(&status);
    mock_check(!status.searching, "simplex wire peer never searches");
    printk("PASS: all %u simplex wire peer status checks\n", (unsigned int)mock_checks_passed());
    exit(0);
}
SYS_INIT(test_wire_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
