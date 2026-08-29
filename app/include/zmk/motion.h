/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zmk/behavior.h>

struct zmk_motion_tap_config {
    bool enabled;
    uint32_t threshold;
    uint32_t time_limit_ms;
    uint32_t latency_ms;
    uint32_t window_ms;
    struct zmk_behavior_binding left_single;
    struct zmk_behavior_binding left_double;
    struct zmk_behavior_binding right_single;
    struct zmk_behavior_binding right_double;
    uint32_t layer_mask;
};

struct zmk_motion_carry_config {
    bool enabled;
    uint32_t motion_threshold;
    uint32_t motion_duration_ms;
};

struct zmk_motion_still_wake_config {
    bool enabled;
    uint32_t settle_duration_ms;
};

struct zmk_motion_sleep_wake_config {
    bool enabled;
    uint32_t threshold;
    uint32_t duration_ms;
};

struct zmk_motion_live_state {
    uint32_t magnitude;
    int32_t orientation;
    bool carry_active;
    bool tap_detected;
    uint8_t last_click_src;
};

int zmk_motion_get_sleep_wake_config(struct zmk_motion_sleep_wake_config *out);
int zmk_motion_set_sleep_wake_config(const struct zmk_motion_sleep_wake_config *cfg);
bool zmk_motion_available(void);
const char *zmk_motion_sensor_name(void);

int zmk_motion_get_tap_config(struct zmk_motion_tap_config *out);
int zmk_motion_set_tap_config(const struct zmk_motion_tap_config *cfg);
int zmk_motion_get_carry_config(struct zmk_motion_carry_config *out);
int zmk_motion_set_carry_config(const struct zmk_motion_carry_config *cfg);
int zmk_motion_get_still_wake_config(struct zmk_motion_still_wake_config *out);
int zmk_motion_set_still_wake_config(const struct zmk_motion_still_wake_config *cfg);

int zmk_motion_save_state(void);
int zmk_motion_settings_reset(void);

int zmk_motion_set_live_stream(bool on);
