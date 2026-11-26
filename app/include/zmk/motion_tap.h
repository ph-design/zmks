/* SPDX-License-Identifier: MIT */

#pragma once

#include <zephyr/drivers/sensor.h>
#include <zmk/motion.h>

#define ZMK_TAP_CAPTURE_FRAMES 32
#define ZMK_TAP_FRAME_SIZE 8
#define ZMK_TAP_REG_COUNT 11

struct zmk_tap_options {
    uint32_t axes;
    uint32_t quiet_ms;
    bool split;
};

struct zmk_tap_diagnostics {
    uint32_t irq, single, dbl, gestures, actions, suppressed, drops;
    uint32_t axes[3], src, odr, test_ms, engine, full_scale_mg;
    uint8_t registers[ZMK_TAP_REG_COUNT];
    int32_t error, current[3];
};

struct zmk_tap_capture {
    uint32_t sequence;
    uint32_t count;
    uint8_t frames[ZMK_TAP_CAPTURE_FRAMES * ZMK_TAP_FRAME_SIZE];
};

void zmk_tap_get_options(struct zmk_tap_options *out);
int zmk_tap_configure(const struct zmk_motion_tap_config *config,
                      const struct zmk_tap_options *options);
bool zmk_tap_config_valid(const struct zmk_motion_tap_config *config);
void zmk_motion_get_tap_config(struct zmk_motion_tap_config *out);
int zmk_motion_set_tap_config(const struct zmk_motion_tap_config *config,
                              const struct zmk_tap_options *options);
int zmk_tap_save(void);
int zmk_tap_reset(void);
void zmk_tap_get_diagnostics(struct zmk_tap_diagnostics *out);
void zmk_tap_get_capture(struct zmk_tap_capture *out);
int zmk_tap_set_test(bool on);
bool zmk_tap_test_active(void);
void zmk_tap_sample(const struct sensor_value xyz[3], int64_t timestamp);
bool zmk_tap_take_click(uint8_t *src);

void zmk_lis2dh_set_double_click_axes(const struct device *dev, uint8_t axes);
int zmk_lis2dh_click_snapshot(const struct device *dev, uint8_t regs[ZMK_TAP_REG_COUNT],
                              uint32_t *full_scale_mg);
void zmk_tap_gpio(void);
void zmk_tap_click(uint8_t src);
