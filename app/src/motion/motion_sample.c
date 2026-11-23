/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zmk/motion_sample.h>
#include <zmk/motion_tap.h>

static const struct device *const imu = DEVICE_DT_GET(DT_CHOSEN(zmk_imu));
static struct zmk_motion_sample latest;
static struct k_spinlock sample_lock;
static atomic_t sampling;
static K_MUTEX_DEFINE(sensor_lock);

struct zmk_motion_sample zmk_motion_sample_get(void) {
    k_spinlock_key_t key = k_spin_lock(&sample_lock);
    struct zmk_motion_sample sample = latest;
    k_spin_unlock(&sample_lock, key);
    return sample;
}

int32_t zmk_motion_sample_mg(const struct sensor_value *value) {
    return sensor_value_to_micro(value) * 1000 / SENSOR_G;
}

void zmk_motion_sample_lock(void) { k_mutex_lock(&sensor_lock, K_FOREVER); }

void zmk_motion_sample_unlock(void) { k_mutex_unlock(&sensor_lock); }

static void sample_main(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    /* Studio also uses these samples when motion actions are disabled. */
    for (;;) {
        if (atomic_get(&sampling)) {
            zmk_motion_sample_lock();
            if (atomic_get(&sampling)) {
                struct zmk_motion_sample sample = {0};
                sample.valid = sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ) == 0 &&
                               sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, sample.xyz) == 0;
                sample.timestamp = k_uptime_get();
                if (IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP) && sample.valid) {
                    zmk_tap_sample(sample.xyz, sample.timestamp);
                }
                k_spinlock_key_t key = k_spin_lock(&sample_lock);
                sample.sequence = latest.sequence + 1;
                latest = sample;
                k_spin_unlock(&sample_lock, key);
            }
            zmk_motion_sample_unlock();
        }
        if (IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP) && zmk_tap_test_active()) {
            k_sleep(K_USEC(2500));
        } else {
            k_sleep(K_MSEC(10));
        }
    }
}

static K_THREAD_DEFINE(motion_sampler, 1536, sample_main, NULL, NULL, NULL, 8, 0, 0);

int zmk_motion_sample_start(void) {
    if (!device_is_ready(imu)) {
        return -ENODEV;
    }
    const struct sensor_value odr = {.val1 = 100};
    zmk_motion_sample_lock();
    int err = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
    if (!err) {
        atomic_set(&sampling, 1);
    }
    zmk_motion_sample_unlock();
    return err;
}

int zmk_motion_sample_suspend(void) {
    if (!atomic_set(&sampling, 0)) {
        return 0;
    }
    int err = k_mutex_lock(&sensor_lock, K_MSEC(100));
    if (err) {
        atomic_set(&sampling, 1);
        return err;
    }
    k_spinlock_key_t key = k_spin_lock(&sample_lock);
    latest.valid = false;
    k_spin_unlock(&sample_lock, key);
    k_mutex_unlock(&sensor_lock);
    return 0;
}

void zmk_motion_sample_resume(void) {
    if (device_is_ready(imu)) {
        atomic_set(&sampling, 1);
    }
}
