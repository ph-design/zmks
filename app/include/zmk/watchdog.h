/*
 * Copyright (c) 2025 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if IS_ENABLED(CONFIG_ZMK_UNRESPONSE_FIX)

void zmk_wdt_feed_now(void);
void zmk_wdt_note_tx_ok(void);
void zmk_wdt_note_tx_fail(void);

#else

static inline void zmk_wdt_feed_now(void) {}
static inline void zmk_wdt_note_tx_ok(void) {}
static inline void zmk_wdt_note_tx_fail(void) {}

#endif
