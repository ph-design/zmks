/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/event_manager.h>
#include <zmk/motion.h>

struct zmk_motion_live_state_changed {
    struct zmk_motion_live_state state;
};

ZMK_EVENT_DECLARE(zmk_motion_live_state_changed);
