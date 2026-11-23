/* SPDX-License-Identifier: MIT */

#pragma once

#include <zephyr/drivers/sensor.h>

struct zmk_motion_sample {
    struct sensor_value xyz[3];
    int64_t timestamp;
    uint32_t sequence;
    bool valid;
};

struct zmk_motion_sample zmk_motion_sample_get(void);
int32_t zmk_motion_sample_mg(const struct sensor_value *value);
int zmk_motion_sample_start(void);
int zmk_motion_sample_suspend(void);
void zmk_motion_sample_resume(void);
void zmk_motion_sample_lock(void);
void zmk_motion_sample_unlock(void);
