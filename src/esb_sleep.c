// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Radio power follows ZMK activity. */

#include <zephyr/sys/atomic.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include "esb_link.h"

static atomic_t asleep;

static int esb_sleep_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }
    if (ev->state == ZMK_ACTIVITY_SLEEP) {
        /* Stop radio before poweroff so SYSTEM_OFF can't cut an active transmit. */
        (void)esb_link_set_enabled(false);
        atomic_set(&asleep, 1);
        return 0;
    }
    /* Poweroff can fail and leave ZMK awake, the next state brings the link back. */
    if (atomic_cas(&asleep, 1, 0)) {
        (void)esb_link_set_enabled(true);
    }
    if (ev->state == ZMK_ACTIVITY_IDLE) {
        esb_link_set_idle(true);
    } else if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        esb_link_set_idle(false);
    }
    return 0;
}

ZMK_LISTENER(esb_sleep, esb_sleep_listener);
ZMK_SUBSCRIPTION(esb_sleep, zmk_activity_state_changed);
