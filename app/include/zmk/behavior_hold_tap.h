/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/behavior.h>

struct zmk_hold_tap_pub_config {
    int32_t tapping_term_ms;
    int32_t require_prior_idle_ms;
    int32_t quick_tap_ms;
    uint8_t flavor;
    bool retro_tap;
    bool hold_while_undecided;
    bool hold_while_undecided_linger;
    bool hold_trigger_on_release;
};

bool zmk_behavior_is_hold_tap(zmk_behavior_local_id_t local_id);

int zmk_behavior_hold_tap_get_config(zmk_behavior_local_id_t local_id,
                                     struct zmk_hold_tap_pub_config *out);

int zmk_behavior_hold_tap_set_config(zmk_behavior_local_id_t local_id,
                                     const struct zmk_hold_tap_pub_config *in);

int zmk_behavior_hold_tap_save_all(void);

int zmk_behavior_hold_tap_settings_reset(void);

int zmk_behavior_hold_tap_reload_from_settings(void);
