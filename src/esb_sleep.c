// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include "esb_link.h"

/* Stop radio before poweroff so SYSTEM_OFF can't cut an active transmit. */
static int esb_sleep_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev != NULL && ev->state == ZMK_ACTIVITY_SLEEP) {
        (void)esb_link_set_enabled(false);
    }
    return 0;
}

ZMK_LISTENER(esb_sleep, esb_sleep_listener);
ZMK_SUBSCRIPTION(esb_sleep, zmk_activity_state_changed);
