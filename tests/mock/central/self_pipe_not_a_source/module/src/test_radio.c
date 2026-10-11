// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake ESB link core on a central whose own half holds a self pipe, one peripheral heard.
 * Exits 0 once the self pipe is no split source and the heard peripheral alone
 * reports every connection made.
 * Exits 1 on a self pipe among the sources or connections short of all.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/central.h>

#include <esb.h>

#include "esb_link.h"
#include "esb_link_internal.h"
#include "hop.h"
#include "mock.h"

#define SELF_PIPE DT_PROP(DT_NODELABEL(left), pipe)
#define PERIPHERAL_PIPE DT_PROP(DT_NODELABEL(right), pipe)
#define VERDICT_MS 1000 /* outlasts one staleness tick */

BUILD_ASSERT(SELF_PIPE != PERIPHERAL_PIPE, "self and peripheral pipes differ");

int esb_write_payload(const struct esb_payload *payload) {
    ARG_UNUSED(payload);
    return 0;
}

int esb_link_init(esb_link_rx_callback_t callback) {
    ARG_UNUSED(callback);
    return 0;
}

int esb_link_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    return 0;
}

int esb_link_hfclk_acquire(void) {
    return 0;
}

void hop_boot_mask(void) {
}

uint32_t hop_pipe_quiet_ms(uint8_t pipe) {
    ARG_UNUSED(pipe);
    return 0;
}

bool hop_pipe_heard(uint8_t pipe) {
    return pipe == PERIPHERAL_PIPE;
}

static struct zmk_split_transport_status transport_status(void) {
    struct zmk_split_transport_status status = {0};
    size_t transports = 0;
    STRUCT_SECTION_FOREACH(zmk_split_transport_central, transport) {
        status = transport->api->get_status();
        transports++;
    }
    mock_check(transports == 1, "one central transport registered");
    return status;
}

static void verdict_fn(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t sources[ESB_LINK_PIPE_MAX] = {0};
    uint8_t source_count = esb_link_source_ids(sources);
    mock_check(source_count == 1, "one split source");
    mock_check(sources[0] == PERIPHERAL_PIPE, "the peripheral pipe is the split source");
    enum zmk_split_transport_connections_status connections = transport_status().connections;
    mock_check(connections == ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED,
               "heard peripheral alone makes every connection");
    printk("PASS: self pipe is no split source, the heard peripheral connects all\n");
    exit(0);
}
static K_WORK_DELAYABLE_DEFINE(verdict_work, verdict_fn);

static int test_radio_init(void) {
    k_work_reschedule(&verdict_work, K_MSEC(VERDICT_MS));
    return 0;
}
SYS_INIT(test_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
