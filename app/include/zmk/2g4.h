/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

int zmk_2g4_send_keyboard_report(void);
int zmk_2g4_send_consumer_report(void);
#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_2g4_send_mouse_report(void);
#endif
bool zmk_2g4_is_ready(void);
int zmk_2g4_start(void);
int zmk_2g4_stop(void);
