/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };

enum zmk_activity_state zmk_activity_get_state(void);

// Enter the idle-timeout sleep path immediately
void zmk_activity_force_sleep(void);
void zmk_activity_note_activity(void);

#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
void zmk_activity_note_key(uint32_t position, bool pressed);
bool zmk_activity_input_recent(uint32_t quiet_ms);
#endif
