// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Radio power follows ZMK activity. */

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include "esb_link.h"

static int esb_sleep_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }
    if (ev->state == ZMK_ACTIVITY_SLEEP) {
        /* Stop radio before poweroff so SYSTEM_OFF can't cut an active transmit. */
        (void)esb_link_set_enabled(false);
    } else if (ev->state == ZMK_ACTIVITY_IDLE) {
        esb_link_set_idle(true);
    } else if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        esb_link_set_idle(false);
    }
    return 0;
}

ZMK_LISTENER(esb_sleep, esb_sleep_listener);
ZMK_SUBSCRIPTION(esb_sleep, zmk_activity_state_changed);
