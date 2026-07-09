// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include <zephyr/ztest.h>

#include "esb_hid_state.h"

ZTEST_SUITE(esb_hid_state, NULL, NULL, NULL, NULL, NULL);

ZTEST(esb_hid_state, test_round_trip) {
    uint16_t state = esb_hid_state_pack(0xA5, 0x3C);
    zassert_equal(esb_hid_state_modifiers(state), 0xA5, "modifiers survive");
    zassert_equal(esb_hid_state_indicators(state), 0x3C, "indicators survive");
}

ZTEST(esb_hid_state, test_fields_do_not_bleed) {
    zassert_equal(esb_hid_state_indicators(esb_hid_state_pack(0xFF, 0x00)), 0x00,
                  "full modifiers leave indicators clear");
    zassert_equal(esb_hid_state_modifiers(esb_hid_state_pack(0x00, 0xFF)), 0x00,
                  "full indicators leave modifiers clear");
}
