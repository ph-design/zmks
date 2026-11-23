/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct zmk_power_config {
    uint32_t idle_timeout_ms;
    uint32_t rgb_off_ms;
    uint32_t backlight_off_ms;
    uint32_t deep_sleep_ms;
    bool on_usb;
    bool off_when_locked;
};

struct zmk_power_state {
    bool idle;
    bool deep_sleep;
};

void zmk_power_get_config(struct zmk_power_config *out);
int zmk_power_set_config(const struct zmk_power_config *config);
int zmk_power_save_config(void);
int zmk_power_reset_config(void);
struct zmk_power_state zmk_power_update(uint32_t inactive_ms, bool held, bool usb_powered,
                                        bool locked, bool studio_unlocked);
bool zmk_power_rgb_off(void);
bool zmk_power_backlight_off(void);
