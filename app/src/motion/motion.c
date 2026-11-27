/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/lis2dh.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/motion_live_state_changed.h>
#include <zmk/motion.h>
#include <zmk/motion_guard.h>
#include <zmk/motion_sample.h>
#include <zmk/motion_tap.h>

#if DT_HAS_CHOSEN(zmk_imu)

#define IMU_DEV DEVICE_DT_GET(DT_CHOSEN(zmk_imu))

#if !IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)
static const struct gpio_dt_spec imu_int2 =
    GPIO_DT_SPEC_GET_BY_IDX(DT_CHOSEN(zmk_imu), irq_gpios, 1);
BUILD_ASSERT(DT_PROP_LEN(DT_CHOSEN(zmk_imu), irq_gpios) >= 2,
             "motion needs two irq-gpios entries on the zmk,imu chosen node");

// walking pace is ~2 Hz shock pulses; a longer gap breaks the streak
#define CARRY_STEP_GAP_MS 2000
#define CARRY_CHECK_PERIOD_MS 500
#endif
#define LIVE_PERIOD_MS 100
// sample at the 50 Hz ODR so impulse peaks between pushes are not missed
#define LIVE_SAMPLE_MS 20
#define LIVE_SAMPLES_PER_PUSH (LIVE_PERIOD_MS / LIVE_SAMPLE_MS)
#define SETTLE_DEFAULTS_MS 5000

// THS registers step 16 mg/LSb at the driver's boot range of ±2g
#define THS_LSB_UMS2 156905

// values match the LiveState.orientation proto enum
enum motion_orientation {
    ORIENT_UNKNOWN = 0,
    ORIENT_FLAT_UP = 1,
    ORIENT_FLAT_DOWN = 2,
    ORIENT_TILTED = 3,
};

// pose from the gravity vector: Z-dominant means flat; a weak Z is free fall
// or no gravity sample yet
static int orientation_from_ums2(int64_t x, int64_t y, int64_t z) {
#if IS_ENABLED(CONFIG_ZMK_MOTION_IMU_FLIP_Z)
    z = -z;
#endif
    int64_t ax = x < 0 ? -x : x;
    int64_t ay = y < 0 ? -y : y;
    int64_t az = z < 0 ? -z : z;

    if (az < 5000000) { // below ~0.5 g
        return ORIENT_UNKNOWN;
    }
    if (az > ax && az > ay) {
        return z > 0 ? ORIENT_FLAT_UP : ORIENT_FLAT_DOWN;
    }
    return ORIENT_TILTED;
}

#define TAP_CONFIG_DEFAULTS                                                                        \
    {.enabled = false,                                                                             \
     .threshold = 40,                                                                              \
     .time_limit_ms = 30,                                                                           \
     .latency_ms = 80,                                                                             \
     .window_ms = 240,                                                                             \
     .layer_mask = 0}
#define CARRY_CONFIG_DEFAULTS {.enabled = true, .motion_threshold = 32, .motion_duration_ms = 60000}
#define STILL_WAKE_CONFIG_DEFAULTS {.enabled = true, .settle_duration_ms = SETTLE_DEFAULTS_MS}
#define SLEEP_WAKE_CONFIG_DEFAULTS {.enabled = false, .threshold = 16, .duration_ms = 10000}

#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
static struct zmk_motion_tap_config tap_cfg = TAP_CONFIG_DEFAULTS;
#endif
static struct zmk_motion_carry_config carry_cfg = CARRY_CONFIG_DEFAULTS;
static struct zmk_motion_still_wake_config still_wake_cfg = STILL_WAKE_CONFIG_DEFAULTS;
static struct zmk_motion_sleep_wake_config sleep_wake_cfg = SLEEP_WAKE_CONFIG_DEFAULTS;
// fields are written from different contexts (trigger thread, work queue)
// with no field shared between them
static struct zmk_motion_live_state live;

// per-axis gravity baseline; magnitude is deviation from it so still reads ~0
static int64_t live_baseline[3];
static bool live_baseline_valid;

// windowed peak state for the 50 Hz sampler; pushed once per LIVE_PERIOD_MS
static uint32_t live_window_peak;
static uint8_t live_sample_count;
static bool live_window_fetched;

static const struct device *imu = IMU_DEV;

#if !IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)
// carry streak state
static int64_t streak_start;
static int64_t last_motion;

// counts any-motion trigger handler invocations for the field diagnostic log
static atomic_t anym_trigger_count;

// set when the settle check put us back to sleep: skip re-arming the IMU
// wake-up, otherwise a keyboard jostled in a bag would wake in a loop
static bool imu_wake_disabled;
static bool settle_armed;

static void settle_check_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(settle_work, settle_check_work_cb);
#endif

static void motion_live_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(live_work, motion_live_work_cb);

#if !IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)
static void carry_check_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(carry_work, carry_check_work_cb);
#endif

#if !IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)
static void carry_trigger_handler(const struct device *dev, const struct sensor_trigger *trig) {
    ARG_UNUSED(dev);
    ARG_UNUSED(trig);

    atomic_inc(&anym_trigger_count);

    int64_t now = k_uptime_get();
    last_motion = now;
    if (streak_start == 0) {
        streak_start = now;
    }
}

static void carry_check_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();

    // field diagnostics: trigger rate and streak state while USB-attached
    static uint32_t tick;
    static uint32_t last_logged;
    if ((tick++ & 3) == 0) {
        uint32_t cnt = atomic_get(&anym_trigger_count);
        LOG_DBG("carry diag: anym/s=%u last_motion_age=%lld streak=%d", (cnt - last_logged) / 2,
                now - last_motion, streak_start != 0);
        last_logged = cnt;
    }

    if (now - last_motion > CARRY_STEP_GAP_MS) {
        streak_start = 0;
        live.carry_active = false;
    } else if (streak_start != 0 && now - streak_start >= carry_cfg.motion_duration_ms) {
        if (!live.carry_active) {
            live.carry_active = true;
            zmk_activity_force_sleep();
        }
    }

    k_work_schedule(&carry_work, K_MSEC(CARRY_CHECK_PERIOD_MS));
}

// Check once after boot whether motion ever settled: if it never went quiet,
// the keyboard is being carried, so go back to sleep with the IMU wake-up
// disarmed.
static void settle_check_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();

    if (!settle_armed) {
        return;
    }
    settle_armed = false;

    bool still_in_motion = streak_start != 0 && (now - last_motion <= CARRY_STEP_GAP_MS);

    if (still_in_motion && carry_cfg.enabled) {
        imu_wake_disabled = true;
        zmk_activity_force_sleep();
    }
}

// Arm the INT2 pin as a System OFF wake source and keep the sensor running;
// the wake-up event is the jolt of the keyboard being set down.
static void prepare_imu_wakeup(void) {
    pm_device_wakeup_enable(imu, true);

    // clear any latched interrupt so INT2 starts low
    struct sensor_value dummy;
    (void)sensor_sample_fetch_chan(imu, SENSOR_CHAN_LIS2DH_ORIENTATION);
    (void)sensor_channel_get(imu, SENSOR_CHAN_LIS2DH_ORIENTATION, &dummy);

    if (imu_wake_disabled || imu_int2.port == NULL) {
        return;
    }

    gpio_pin_configure_dt(&imu_int2, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&imu_int2, GPIO_INT_LEVEL_HIGH);
}

static int motion_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev && ev->state == ZMK_ACTIVITY_SLEEP) {
        prepare_imu_wakeup();
    }

    return 0;
}

ZMK_LISTENER(zmk_motion_activity, motion_activity_listener);
ZMK_SUBSCRIPTION(zmk_motion_activity, zmk_activity_state_changed);

static int carry_key_veto_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    streak_start = 0;
    live.carry_active = false;
    settle_armed = false;
    return 0;
}

ZMK_LISTENER(zmk_motion_carry_veto, carry_key_veto_listener);
ZMK_SUBSCRIPTION(zmk_motion_carry_veto, zmk_position_state_changed);
#endif

static void motion_live_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

#if IS_ENABLED(CONFIG_ZMK_MOTION_SAMPLER)
    const struct zmk_motion_sample sample = zmk_motion_sample_get();
    const struct sensor_value *xyz = sample.xyz;
    if (sample.valid) {
#else
    struct sensor_value xyz[3];
    if (sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ) == 0 &&
        sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, xyz) == 0) {
#endif
        live_window_fetched = true;
        int64_t axis[3];
        for (int i = 0; i < 3; i++) {
            axis[i] = (int64_t)xyz[i].val1 * 1000000 + xyz[i].val2;
            if (!live_baseline_valid) {
                live_baseline[i] = axis[i];
            } else {
                // slow EMA follows reorientation but not vibration (tau ~1.6 s)
                live_baseline[i] += (axis[i] - live_baseline[i]) / 80;
            }
            int64_t dev = axis[i] - live_baseline[i];
            if (dev < 0) {
                dev = -dev;
            }
            uint32_t counts = (uint32_t)(dev / THS_LSB_UMS2);
            if (counts > live_window_peak) {
                live_window_peak = counts;
            }
        }
        live_baseline_valid = true;
    }

    live_sample_count++;
    if (live_sample_count >= LIVE_SAMPLES_PER_PUSH) {
        live_sample_count = 0;
        if (live_window_fetched) {
            if (IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)) {
                live.tap_detected = zmk_tap_take_click(&live.last_click_src);
            }
            if (IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)) {
                live.carry_active = zmk_motion_guard_locked();
            }
            live.magnitude = live_window_peak;
            live.orientation =
                orientation_from_ums2(live_baseline[0], live_baseline[1], live_baseline[2]);
            raise_zmk_motion_live_state_changed(
                (struct zmk_motion_live_state_changed){.state = live});
        }
        live.tap_detected = false;
        live_window_peak = 0;
        live_window_fetched = false;
    }

    k_work_schedule(&live_work, K_MSEC(LIVE_SAMPLE_MS));
}

#if !IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)
static void counts_to_ms2(uint32_t counts, struct sensor_value *v) {
    uint32_t ums2 = counts * THS_LSB_UMS2;
    v->val1 = ums2 / 1000000;
    v->val2 = ums2 % 1000000;
}
#endif

static int apply_tap_settings(void) {
#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
    struct zmk_tap_options opts;
    zmk_tap_get_options(&opts);
    return zmk_tap_configure(&tap_cfg, &opts);
#else
    return 0;
#endif
}

static int apply_sleep_wake_hw(void) {
    struct sensor_value v;
    int rc;

    v.val1 = sleep_wake_cfg.enabled && !IS_ENABLED(CONFIG_ZMK_MOTION_SAMPLER)
                 ? sleep_wake_cfg.threshold
                 : 0;
    v.val2 = 0;
    rc = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_LIS2DH_ACT_THS, &v);
    if (rc < 0) {
        return rc;
    }

    v.val1 = sleep_wake_cfg.duration_ms;
    return sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_LIS2DH_ACT_DUR_MS, &v);
}

static int apply_carry_hw(void) {
#if IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)
    zmk_motion_guard_set_enabled(carry_cfg.enabled);
    return 0;
#else
    static const struct sensor_trigger delta_trig = {.type = SENSOR_TRIG_DELTA,
                                                     .chan = SENSOR_CHAN_ACCEL_XYZ};
    struct sensor_value th;
    counts_to_ms2(carry_cfg.motion_threshold, &th);
    int rc = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLOPE_TH, &th);
    if (rc < 0) {
        return rc;
    }

    streak_start = 0;
    last_motion = 0;
    live.carry_active = false;

    sensor_trigger_set(imu, &delta_trig, carry_cfg.enabled ? carry_trigger_handler : NULL);
    if (carry_cfg.enabled) {
        k_work_schedule(&carry_work, K_MSEC(CARRY_CHECK_PERIOD_MS));
    } else {
        k_work_cancel_delayable(&carry_work);
        k_work_cancel_delayable(&settle_work);
        settle_armed = false;
    }
    return 0;
#endif
}

static void arm_boot_settle_check(void) {
#if !IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)
    if (carry_cfg.enabled && still_wake_cfg.enabled) {
        settle_armed = true;
        k_work_schedule(&settle_work, K_MSEC(still_wake_cfg.settle_duration_ms));
    }
#endif
}

static int apply_motion_settings(void) {
    int err = apply_tap_settings();
    apply_carry_hw();
    apply_sleep_wake_hw();
    return err;
}

bool zmk_motion_available(void) { return device_is_ready(imu); }

const char *zmk_motion_sensor_name(void) { return imu->name; }

#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
void zmk_motion_get_tap_config(struct zmk_motion_tap_config *out) { *out = tap_cfg; }

int zmk_motion_set_tap_config(const struct zmk_motion_tap_config *cfg,
                              const struct zmk_tap_options *opts) {
    int err = zmk_tap_configure(cfg, opts);
    if (!err) {
        tap_cfg = *cfg;
    }
    return err;
}
#endif

int zmk_motion_get_carry_config(struct zmk_motion_carry_config *out) {
    *out = carry_cfg;
    return 0;
}

int zmk_motion_set_carry_config(const struct zmk_motion_carry_config *cfg) {
    if (cfg->motion_threshold > 127) {
        return -EINVAL;
    }

    carry_cfg = *cfg;
    return apply_carry_hw();
}

int zmk_motion_get_still_wake_config(struct zmk_motion_still_wake_config *out) {
    *out = still_wake_cfg;
    return 0;
}

int zmk_motion_set_still_wake_config(const struct zmk_motion_still_wake_config *cfg) {
#if IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)
    return -ENOTSUP;
#else
    still_wake_cfg = *cfg;

    if (!cfg->enabled) {
        k_work_cancel_delayable(&settle_work);
        settle_armed = false;
    }
    return 0;
#endif
}

int zmk_motion_get_sleep_wake_config(struct zmk_motion_sleep_wake_config *out) {
    *out = sleep_wake_cfg;
    return 0;
}

int zmk_motion_set_sleep_wake_config(const struct zmk_motion_sleep_wake_config *cfg) {
    if (IS_ENABLED(CONFIG_ZMK_MOTION_SAMPLER)) {
        return -ENOTSUP;
    }
    if (cfg->threshold > 127) {
        return -EINVAL;
    }

    sleep_wake_cfg = *cfg;
    return apply_sleep_wake_hw();
}

int zmk_motion_set_live_stream(bool on) {
    if (on) {
        live_baseline_valid = false;
        live_sample_count = 0;
        live_window_peak = 0;
        live_window_fetched = false;
        k_work_schedule(&live_work, K_NO_WAIT);
    } else {
        k_work_cancel_delayable(&live_work);
    }
    return 0;
}

#define MOTION_SETTINGS_VERSION 1

#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
#define MOTION_TAP_SETTINGS_KEY "double_tap"

struct motion_tap_blob {
    uint8_t version;
    uint8_t enabled;
    uint16_t threshold;
    uint32_t time_limit_ms;
    uint32_t latency_ms;
    uint32_t window_ms;
    uint32_t layer_mask;
    zmk_behavior_local_id_t local_ids[2];
    uint32_t param1[2];
    uint32_t param2[2];
} __packed;

/* A nonzero version marks a record staged for this settings commit. */
static struct motion_tap_blob pending_tap;
#endif

struct motion_carry_blob {
    uint8_t version;
    uint8_t enabled;
    uint16_t _pad;
    uint32_t motion_threshold;
    uint32_t motion_duration_ms;
} __packed;

struct motion_still_wake_blob {
    uint8_t version;
    uint8_t enabled;
    uint16_t _pad;
    uint32_t settle_duration_ms;
} __packed;

struct motion_sleep_wake_blob {
    uint8_t version;
    uint8_t enabled;
    uint16_t _pad;
    uint32_t threshold;
    uint32_t duration_ms;
} __packed;

static void carry_defaults(void) {
    carry_cfg = (struct zmk_motion_carry_config)CARRY_CONFIG_DEFAULTS;
}

static void still_wake_defaults(void) {
    still_wake_cfg = (struct zmk_motion_still_wake_config)STILL_WAKE_CONFIG_DEFAULTS;
}

static void sleep_wake_defaults(void) {
    sleep_wake_cfg = (struct zmk_motion_sleep_wake_config)SLEEP_WAKE_CONFIG_DEFAULTS;
}

#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
static bool binding_is_set(const struct zmk_behavior_binding *binding) { return binding->behavior_dev != NULL; }

static void binding_to_store(const struct zmk_behavior_binding *b, int idx,
                             struct motion_tap_blob *blob) {
    blob->local_ids[idx] =
        binding_is_set(b) ? zmk_behavior_get_local_id(b->behavior_dev) : UINT16_MAX;
    blob->param1[idx] = binding_is_set(b) ? b->param1 : 0;
    blob->param2[idx] = binding_is_set(b) ? b->param2 : 0;
}

static bool binding_from_store(struct zmk_behavior_binding *b, int idx,
                               const struct motion_tap_blob *blob) {
    const char *name = zmk_behavior_find_behavior_name_from_local_id(blob->local_ids[idx]);
    if (!name) {
        *b = (struct zmk_behavior_binding){0};
        return blob->local_ids[idx] == UINT16_MAX;
    }
    *b = (struct zmk_behavior_binding){
        .behavior_dev = name,
        .local_id = blob->local_ids[idx],
        .param1 = blob->param1[idx],
        .param2 = blob->param2[idx],
    };
    return true;
}

static void restore_pending_tap(void) {
    if (!pending_tap.version) {
        return;
    }

    struct zmk_motion_tap_config loaded = {
        .enabled = pending_tap.enabled,
        .threshold = pending_tap.threshold,
        .time_limit_ms = pending_tap.time_limit_ms,
        .latency_ms = pending_tap.latency_ms,
        .window_ms = pending_tap.window_ms,
        .layer_mask = pending_tap.layer_mask,
    };
    if (zmk_tap_config_valid(&loaded) &&
        binding_from_store(&loaded.left_double, 0, &pending_tap) &&
        binding_from_store(&loaded.right_double, 1, &pending_tap)) {
        tap_cfg = loaded;
    } else {
        LOG_WRN("Ignoring invalid saved double-tap configuration");
    }
    pending_tap.version = 0;
}

#endif

int zmk_motion_save_state(void) {
    int rc;
#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
    struct motion_tap_blob tb = {.version = MOTION_SETTINGS_VERSION,
                                 .enabled = tap_cfg.enabled ? 1 : 0,
                                 .threshold = (uint16_t)tap_cfg.threshold,
                                 .time_limit_ms = tap_cfg.time_limit_ms,
                                 .latency_ms = tap_cfg.latency_ms,
                                 .window_ms = tap_cfg.window_ms,
                                 .layer_mask = tap_cfg.layer_mask};
    binding_to_store(&tap_cfg.left_double, 0, &tb);
    binding_to_store(&tap_cfg.right_double, 1, &tb);

    rc = settings_save_one("motion/" MOTION_TAP_SETTINGS_KEY, &tb, sizeof(tb));
    if (rc < 0) {
        return rc;
    }
#endif

    struct motion_carry_blob cb = {.version = MOTION_SETTINGS_VERSION,
                                   .enabled = carry_cfg.enabled ? 1 : 0,
                                   .motion_threshold = carry_cfg.motion_threshold,
                                   .motion_duration_ms = carry_cfg.motion_duration_ms};
    rc = settings_save_one("motion/carry", &cb, sizeof(cb));
    if (rc < 0) {
        return rc;
    }

    struct motion_still_wake_blob sw = {.version = MOTION_SETTINGS_VERSION,
                                        .enabled = still_wake_cfg.enabled ? 1 : 0,
                                        .settle_duration_ms = still_wake_cfg.settle_duration_ms};
    rc = settings_save_one("motion/still", &sw, sizeof(sw));
    if (rc < 0) {
        return rc;
    }

    struct motion_sleep_wake_blob wk = {.version = MOTION_SETTINGS_VERSION,
                                        .enabled = sleep_wake_cfg.enabled ? 1 : 0,
                                        .threshold = sleep_wake_cfg.threshold,
                                        .duration_ms = sleep_wake_cfg.duration_ms};
    return settings_save_one("motion/slpwk", &wk, sizeof(wk));
}

int zmk_motion_settings_reset(void) {
    static const char *const keys[] = {
#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
        "motion/" MOTION_TAP_SETTINGS_KEY,
#endif
        "motion/carry",
        "motion/still",
        "motion/slpwk",
    };
    int first_err = 0;

    for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
        int err = settings_delete(keys[i]);
        if (err < 0 && err != -ENOENT && !first_err) {
            first_err = err;
        }
    }

#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
    pending_tap.version = 0;
    tap_cfg = (struct zmk_motion_tap_config)TAP_CONFIG_DEFAULTS;
#endif
    carry_defaults();
    still_wake_defaults();
    sleep_wake_defaults();
    int err = apply_motion_settings();
    return first_err ? first_err : err;
}

static int motion_settings_load(const char *name, size_t len, settings_read_cb read_cb,
                                void *cb_arg) {
#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
    if (strcmp(name, MOTION_TAP_SETTINGS_KEY) == 0) {
        if (len != sizeof(pending_tap) ||
            read_cb(cb_arg, &pending_tap, sizeof(pending_tap)) != sizeof(pending_tap) ||
            pending_tap.version != MOTION_SETTINGS_VERSION || pending_tap.enabled > 1) {
            pending_tap.version = 0;
            return -EINVAL;
        }
        return 0;
    }
#endif

    if (strcmp(name, "carry") == 0) {
        struct motion_carry_blob cb;
        if (len != sizeof(cb) || read_cb(cb_arg, &cb, sizeof(cb)) != sizeof(cb) ||
            cb.version != MOTION_SETTINGS_VERSION) {
            return -EINVAL;
        }
        carry_cfg.enabled = cb.enabled != 0;
        carry_cfg.motion_threshold = cb.motion_threshold;
        carry_cfg.motion_duration_ms = cb.motion_duration_ms;
        return 0;
    }

    if (strcmp(name, "still") == 0) {
        struct motion_still_wake_blob sw;
        if (len != sizeof(sw) || read_cb(cb_arg, &sw, sizeof(sw)) != sizeof(sw) ||
            sw.version != MOTION_SETTINGS_VERSION) {
            return -EINVAL;
        }
        still_wake_cfg.enabled = sw.enabled != 0;
        still_wake_cfg.settle_duration_ms = sw.settle_duration_ms;
        return 0;
    }

    if (strcmp(name, "slpwk") == 0) {
        struct motion_sleep_wake_blob wk;
        if (len != sizeof(wk) || read_cb(cb_arg, &wk, sizeof(wk)) != sizeof(wk) ||
            wk.version != MOTION_SETTINGS_VERSION) {
            return -EINVAL;
        }
        sleep_wake_cfg.enabled = wk.enabled != 0;
        sleep_wake_cfg.threshold = wk.threshold;
        sleep_wake_cfg.duration_ms = wk.duration_ms;
        return 0;
    }

    return -ENOENT;
}

// runs after settings_load() from main(); re-apply the loaded config
static int motion_settings_commit(void) {
#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
    restore_pending_tap();
#endif
    if (!device_is_ready(imu)) {
        return 0; // zmk_motion_init applies once the device comes up
    }

    int tap_err = apply_motion_settings();
    arm_boot_settle_check();
    return tap_err;
}

/* NVS may deliver motion/double_tap before the saved behavior IDs. Resolve its
 * bindings after the behavior table's default-priority (0) commit. */
SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(zmk_motion, "motion", NULL, motion_settings_load,
                                          motion_settings_commit, NULL, 1);

static int zmk_motion_init(void) {
    if (!device_is_ready(imu)) {
        LOG_ERR("IMU device not ready");
        return -ENODEV;
    }

    // runs before settings_load (SYS_INIT < main); values loaded from settings
    // are applied again by the commit handler
    int tap_err = apply_motion_settings();
    arm_boot_settle_check();

#if IS_ENABLED(CONFIG_ZMK_MOTION_SAMPLER)
    int sample_err = zmk_motion_sample_start();
    if (sample_err) {
        return sample_err;
    }
#endif
    return tap_err;
}

SYS_INIT(zmk_motion_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else /* !DT_HAS_CHOSEN(zmk_imu) */

bool zmk_motion_available(void) { return false; }

const char *zmk_motion_sensor_name(void) { return ""; }

int zmk_motion_get_carry_config(struct zmk_motion_carry_config *out) { return -ENOTSUP; }
int zmk_motion_set_carry_config(const struct zmk_motion_carry_config *cfg) { return -ENOTSUP; }
int zmk_motion_get_still_wake_config(struct zmk_motion_still_wake_config *out) { return -ENOTSUP; }
int zmk_motion_set_still_wake_config(const struct zmk_motion_still_wake_config *cfg) {
    return -ENOTSUP;
}
int zmk_motion_get_sleep_wake_config(struct zmk_motion_sleep_wake_config *out) { return -ENOTSUP; }
int zmk_motion_set_sleep_wake_config(const struct zmk_motion_sleep_wake_config *cfg) {
    return -ENOTSUP;
}

int zmk_motion_save_state(void) { return -ENOTSUP; }
int zmk_motion_settings_reset(void) { return -ENOTSUP; }

int zmk_motion_set_live_stream(bool on) { return -ENOTSUP; }

#endif /* DT_HAS_CHOSEN(zmk_imu) */
