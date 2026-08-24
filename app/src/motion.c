/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/lis2de12.h>
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
#include <zmk/keymap.h>
#include <zmk/motion.h>
#include <zmk/virtual_key_position.h>

#if DT_HAS_CHOSEN(zmk_imu)

#define IMU_DEV DEVICE_DT_GET(DT_CHOSEN(zmk_imu))

static const struct gpio_dt_spec imu_int2 =
    GPIO_DT_SPEC_GET_BY_IDX(DT_CHOSEN(zmk_imu), irq_gpios, 1);
BUILD_ASSERT(DT_PROP_LEN(DT_CHOSEN(zmk_imu), irq_gpios) >= 2,
             "motion needs two irq-gpios entries on the zmk,imu chosen node");

// walking pace is ~2 Hz shock pulses; a longer gap breaks the streak
#define CARRY_STEP_GAP_MS 2000
#define CARRY_CHECK_PERIOD_MS 500
#define LIVE_PERIOD_MS 100
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

// Pose from the gravity vector: Z-dominant means flat, a weak Z is free
// fall or no gravity sample yet. The LIS2DE12 is mounted with its +Z axis
// facing down, so at rest the chip reads gravity along +Z: a negative Z
// sample means the keys face up (flat up), a positive one means the
// keyboard is upside down (flat down).
static int orientation_from_ums2(int64_t x, int64_t y, int64_t z) {
    int64_t ax = x < 0 ? -x : x;
    int64_t ay = y < 0 ? -y : y;
    int64_t az = z < 0 ? -z : z;

    if (az < 5000000) { // below ~0.5 g
        return ORIENT_UNKNOWN;
    }
    if (az > ax && az > ay) {
        return z > 0 ? ORIENT_FLAT_DOWN : ORIENT_FLAT_UP;
    }
    return ORIENT_TILTED;
}

enum tap_side { TAP_LEFT, TAP_RIGHT, TAP_SIDE_COUNT };

// single source for boot defaults and settings reset
#define TAP_CONFIG_DEFAULTS                                                                      \
    {.enabled = false, .threshold = 40, .time_limit_ms = 60, .latency_ms = 80, .window_ms = 240, \
     .layer_mask = 0}
#define CARRY_CONFIG_DEFAULTS \
    {.enabled = true, .motion_threshold = 32, .motion_duration_ms = 60000}
#define STILL_WAKE_CONFIG_DEFAULTS {.enabled = true, .settle_duration_ms = SETTLE_DEFAULTS_MS}

static struct zmk_motion_tap_config tap_cfg = TAP_CONFIG_DEFAULTS;
static struct zmk_motion_carry_config carry_cfg = CARRY_CONFIG_DEFAULTS;
static struct zmk_motion_still_wake_config still_wake_cfg = STILL_WAKE_CONFIG_DEFAULTS;
// fields are written from different contexts (trigger thread, work queue)
// with no field shared between them
static struct zmk_motion_live_state live;

static const struct device *imu = IMU_DEV;

// carry streak state
static int64_t streak_start;
static int64_t last_motion;

// set when the settle check or carry detection put the keyboard back to
// sleep: skip re-arming the IMU wake-up, otherwise a keyboard jostled in a
// bag would wake in a loop
static bool imu_wake_disabled;
static bool settle_armed;

static void settle_check_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(settle_work, settle_check_work_cb);

// per-side single-tap state held back for one double-tap window
struct pending_single {
    struct k_work_delayable work;
    struct zmk_behavior_binding binding;
    atomic_t armed;
};

static void single_fire_work_cb(struct k_work *work);

static struct pending_single pending_singles[TAP_SIDE_COUNT] = {
    [TAP_LEFT] = {.work = Z_WORK_DELAYABLE_INITIALIZER(single_fire_work_cb)},
    [TAP_RIGHT] = {.work = Z_WORK_DELAYABLE_INITIALIZER(single_fire_work_cb)},
};

static void motion_live_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(live_work, motion_live_work_cb);

static void carry_check_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(carry_work, carry_check_work_cb);

static bool binding_is_set(const struct zmk_behavior_binding *b) {
    return b && b->behavior_dev != NULL;
}

static struct zmk_behavior_binding *tap_binding_for(enum tap_side side, bool dbl) {
    if (side == TAP_LEFT) {
        return dbl ? &tap_cfg.left_double : &tap_cfg.left_single;
    }
    return dbl ? &tap_cfg.right_double : &tap_cfg.right_single;
}

static bool tap_layer_active(void) {
    return tap_cfg.layer_mask == 0 || (zmk_keymap_layer_state() & tap_cfg.layer_mask) != 0;
}

static void fire_binding(struct zmk_behavior_binding *b, enum tap_side side) {
    if (!binding_is_set(b) || !tap_layer_active()) {
        return;
    }

    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_MOTION(side),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    live.tap_detected = true;
    zmk_behavior_invoke_binding(b, event, true);
    zmk_behavior_invoke_binding(b, event, false);
}

static enum tap_side click_side(uint8_t src) {
    bool neg = (src & LIS2DE12_CLICK_SRC_SIGN) != 0;
#if IS_ENABLED(CONFIG_ZMK_MOTION_TAP_SIDE_INVERT)
    neg = !neg;
#endif
    return neg ? TAP_LEFT : TAP_RIGHT;
}

static void read_click_src(void) {
    struct sensor_value v;

    if (sensor_channel_get(imu, SENSOR_CHAN_LIS2DE12_CLICK_SRC, &v) == 0) {
        live.last_click_src = (uint8_t)v.val1;
    }
}

static void tap_trigger_handler(const struct device *dev, const struct sensor_trigger *trig) {
    ARG_UNUSED(dev);
    ARG_UNUSED(trig);

    read_click_src();

    enum tap_side side = click_side(live.last_click_src);
    struct zmk_behavior_binding *single = tap_binding_for(side, false);

    if (!binding_is_set(single)) {
        return;
    }

    if (!binding_is_set(tap_binding_for(side, true))) {
        fire_binding(single, side);
        return;
    }

    // a double is armed on this side, hold the single back for one window
    struct pending_single *p = &pending_singles[side];
    p->binding = *single;
    atomic_set(&p->armed, 1);
    k_work_reschedule(&p->work, K_MSEC(tap_cfg.window_ms));
}

static void dtap_trigger_handler(const struct device *dev, const struct sensor_trigger *trig) {
    ARG_UNUSED(dev);
    ARG_UNUSED(trig);

    read_click_src();

    enum tap_side side = click_side(live.last_click_src);
    struct pending_single *p = &pending_singles[side];

    atomic_set(&p->armed, 0);
    k_work_cancel_delayable(&p->work);

    fire_binding(tap_binding_for(side, true), side);
}

static void single_fire_work_cb(struct k_work *work) {
    struct pending_single *p =
        CONTAINER_OF(k_work_delayable_from_work(work), struct pending_single, work);

    if (atomic_cas(&p->armed, 1, 0)) {
        fire_binding(&p->binding, (enum tap_side)(p - pending_singles));
    }
}

static void carry_trigger_handler(const struct device *dev, const struct sensor_trigger *trig) {
    ARG_UNUSED(dev);
    ARG_UNUSED(trig);

    int64_t now = k_uptime_get();
    last_motion = now;
    if (streak_start == 0) {
        streak_start = now;
    }
}

static void carry_check_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();

    /* settings may disable carry while a check is already in flight */
    if (!carry_cfg.enabled) {
        return;
    }

    if (now - last_motion > CARRY_STEP_GAP_MS) {
        streak_start = 0;
        if (live.carry_active) {
            live.carry_active = false;
            raise_zmk_motion_live_state_changed(
                (struct zmk_motion_live_state_changed){.state = live});
        }
    } else if (streak_start != 0 && now - streak_start >= carry_cfg.motion_duration_ms) {
        if (!live.carry_active) {
            live.carry_active = true;
            raise_zmk_motion_live_state_changed(
                (struct zmk_motion_live_state_changed){.state = live});
            /* stay asleep in a bag: motion jolts must not wake the
             * keyboard again and again */
            imu_wake_disabled = true;
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
    (void)sensor_sample_fetch_chan(imu, SENSOR_CHAN_LIS2DE12_ORIENTATION);
    (void)sensor_channel_get(imu, SENSOR_CHAN_LIS2DE12_ORIENTATION, &dummy);

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
    /* a key press means the keyboard is in use again: re-arm the IMU
     * wake-up for the next sleep */
    imu_wake_disabled = false;
    return 0;
}

ZMK_LISTENER(zmk_motion_carry_veto, carry_key_veto_listener);
ZMK_SUBSCRIPTION(zmk_motion_carry_veto, zmk_position_state_changed);

static void motion_live_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    struct sensor_value xyz[3];
    uint32_t peak = 0;
    bool fetched = false;

    if (sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ) == 0 &&
        sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, xyz) == 0) {
        fetched = true;
        int64_t axis[3];
        for (int i = 0; i < 3; i++) {
            int64_t ums2 = (int64_t)xyz[i].val1 * 1000000 + xyz[i].val2;
            axis[i] = ums2;
            if (ums2 < 0) {
                ums2 = -ums2;
            }
            uint32_t counts = (uint32_t)(ums2 / THS_LSB_UMS2);
            if (counts > peak) {
                peak = counts;
            }
        }
        live.magnitude = peak;
        live.orientation = orientation_from_ums2(axis[0], axis[1], axis[2]);
    }

    if (fetched) {
        raise_zmk_motion_live_state_changed((struct zmk_motion_live_state_changed){.state = live});
    }
    live.tap_detected = false;

    k_work_schedule(&live_work, K_MSEC(LIVE_PERIOD_MS));
}

static void counts_to_ms2(uint32_t counts, struct sensor_value *v) {
    uint32_t ums2 = counts * THS_LSB_UMS2;
    v->val1 = ums2 / 1000000;
    v->val2 = ums2 % 1000000;
}

static int apply_tap_hw(void) {
    struct sensor_value v;
    int rc;

    v.val1 = tap_cfg.threshold;
    v.val2 = 0;
    rc = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_LIS2DE12_CLICK_THS, &v);
    if (rc < 0) {
        return rc;
    }

    v.val1 = tap_cfg.time_limit_ms;
    rc = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_LIS2DE12_CLICK_TIME_LIMIT_MS, &v);
    if (rc < 0) {
        return rc;
    }

    v.val1 = tap_cfg.latency_ms;
    rc = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_LIS2DE12_CLICK_LATENCY_MS, &v);
    if (rc < 0) {
        return rc;
    }

    v.val1 = tap_cfg.window_ms;
    return sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_LIS2DE12_CLICK_WINDOW_MS, &v);
}

static void apply_tap_triggers(void) {
    static const struct sensor_trigger tap_trig = {
        .type = SENSOR_TRIG_TAP, .chan = SENSOR_CHAN_ACCEL_XYZ};
    static const struct sensor_trigger dtap_trig = {
        .type = SENSOR_TRIG_DOUBLE_TAP, .chan = SENSOR_CHAN_ACCEL_XYZ};

    bool on = tap_cfg.enabled &&
              (binding_is_set(&tap_cfg.left_single) || binding_is_set(&tap_cfg.left_double) ||
               binding_is_set(&tap_cfg.right_single) || binding_is_set(&tap_cfg.right_double));

    sensor_trigger_set(imu, &tap_trig, on ? tap_trigger_handler : NULL);
    sensor_trigger_set(imu, &dtap_trig, on ? dtap_trigger_handler : NULL);
}

static int apply_carry_hw(void) {
    static const struct sensor_trigger delta_trig = {
        .type = SENSOR_TRIG_DELTA, .chan = SENSOR_CHAN_ACCEL_XYZ};

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

        // with still-wake on, watch the post-boot settle window
        if (still_wake_cfg.enabled) {
            settle_armed = true;
            k_work_schedule(&settle_work, K_MSEC(still_wake_cfg.settle_duration_ms));
        }
    } else {
        k_work_cancel_delayable(&carry_work);
        k_work_cancel_delayable(&settle_work);
        settle_armed = false;
        imu_wake_disabled = false;
    }
    return 0;
}

bool zmk_motion_available(void) { return device_is_ready(imu); }

const char *zmk_motion_sensor_name(void) { return imu->name; }

int zmk_motion_get_tap_config(struct zmk_motion_tap_config *out) {
    *out = tap_cfg;
    return 0;
}

int zmk_motion_set_tap_config(const struct zmk_motion_tap_config *cfg) {
    if (cfg->threshold > 127) {
        return -EINVAL;
    }

    tap_cfg = *cfg;
    for (int i = 0; i < TAP_SIDE_COUNT; i++) {
        atomic_set(&pending_singles[i].armed, 0);
        k_work_cancel_delayable(&pending_singles[i].work);
    }
    int rc = apply_tap_hw();
    apply_tap_triggers();
    return rc;
}

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
    still_wake_cfg = *cfg;

    if (cfg->enabled && carry_cfg.enabled) {
        settle_armed = true;
        k_work_schedule(&settle_work, K_MSEC(cfg->settle_duration_ms));
    } else {
        k_work_cancel_delayable(&settle_work);
        settle_armed = false;
    }
    return 0;
}

int zmk_motion_set_live_stream(bool on) {
    if (on) {
        k_work_schedule(&live_work, K_NO_WAIT);
    } else {
        k_work_cancel_delayable(&live_work);
    }
    return 0;
}

#define MOTION_SETTINGS_VERSION 1

struct motion_tap_blob {
    uint8_t version;
    uint8_t enabled;
    uint16_t threshold;
    uint32_t time_limit_ms;
    uint32_t latency_ms;
    uint32_t window_ms;
    uint32_t layer_mask;
    zmk_behavior_local_id_t local_ids[4];
    uint32_t param1[4];
    uint32_t param2[4];
} __packed;

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

static void tap_defaults(void) { tap_cfg = (struct zmk_motion_tap_config)TAP_CONFIG_DEFAULTS; }

static void carry_defaults(void) {
    carry_cfg = (struct zmk_motion_carry_config)CARRY_CONFIG_DEFAULTS;
}

static void still_wake_defaults(void) {
    still_wake_cfg = (struct zmk_motion_still_wake_config)STILL_WAKE_CONFIG_DEFAULTS;
}

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

int zmk_motion_save_state(void) {
    struct motion_tap_blob tb = {.version = MOTION_SETTINGS_VERSION,
                                 .enabled = tap_cfg.enabled ? 1 : 0,
                                 .threshold = (uint16_t)tap_cfg.threshold,
                                 .time_limit_ms = tap_cfg.time_limit_ms,
                                 .latency_ms = tap_cfg.latency_ms,
                                 .window_ms = tap_cfg.window_ms,
                                 .layer_mask = tap_cfg.layer_mask};
    binding_to_store(&tap_cfg.left_single, 0, &tb);
    binding_to_store(&tap_cfg.left_double, 1, &tb);
    binding_to_store(&tap_cfg.right_single, 2, &tb);
    binding_to_store(&tap_cfg.right_double, 3, &tb);

    int rc = settings_save_one("motion/tap", &tb, sizeof(tb));
    if (rc < 0) {
        return rc;
    }

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
    return settings_save_one("motion/still", &sw, sizeof(sw));
}

int zmk_motion_settings_reset(void) {
    int rc = settings_delete("motion/tap");
    int rc2 = settings_delete("motion/carry");
    int rc3 = settings_delete("motion/still");
    tap_defaults();
    carry_defaults();
    still_wake_defaults();
    apply_tap_hw();
    apply_tap_triggers();
    apply_carry_hw();
    return rc < 0 ? rc : (rc2 < 0 ? rc2 : rc3);
}

static int motion_settings_load(const char *name, size_t len, settings_read_cb read_cb,
                                void *cb_arg) {
    if (strcmp(name, "tap") == 0) {
        struct motion_tap_blob tb;
        if (len != sizeof(tb) ||
            read_cb(cb_arg, &tb, sizeof(tb)) != sizeof(tb) ||
            tb.version != MOTION_SETTINGS_VERSION) {
            return -EINVAL;
        }
        tap_cfg.enabled = tb.enabled != 0;
        tap_cfg.threshold = tb.threshold;
        tap_cfg.time_limit_ms = tb.time_limit_ms;
        tap_cfg.latency_ms = tb.latency_ms;
        tap_cfg.window_ms = tb.window_ms;
        tap_cfg.layer_mask = tb.layer_mask;
        // no short-circuit: all four bindings must be populated even if one is unknown
        bool ok = binding_from_store(&tap_cfg.left_single, 0, &tb) &
                  binding_from_store(&tap_cfg.left_double, 1, &tb) &
                  binding_from_store(&tap_cfg.right_single, 2, &tb) &
                  binding_from_store(&tap_cfg.right_double, 3, &tb);
        return ok ? 0 : -EINVAL;
    }

    if (strcmp(name, "carry") == 0) {
        struct motion_carry_blob cb;
        if (len != sizeof(cb) ||
            read_cb(cb_arg, &cb, sizeof(cb)) != sizeof(cb) ||
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
        if (len != sizeof(sw) ||
            read_cb(cb_arg, &sw, sizeof(sw)) != sizeof(sw) ||
            sw.version != MOTION_SETTINGS_VERSION) {
            return -EINVAL;
        }
        still_wake_cfg.enabled = sw.enabled != 0;
        still_wake_cfg.settle_duration_ms = sw.settle_duration_ms;
        return 0;
    }

    return -ENOENT;
}

// runs after settings_load() from main(); re-apply the loaded config
static int motion_settings_commit(void) {
    if (!device_is_ready(imu)) {
        return 0; // zmk_motion_init applies once the device comes up
    }

    apply_tap_hw();
    apply_tap_triggers();
    apply_carry_hw();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_motion, "motion", NULL, motion_settings_load,
                               motion_settings_commit, NULL);

static int zmk_motion_init(void) {
    if (!device_is_ready(imu)) {
        LOG_ERR("IMU device not ready");
        return -ENODEV;
    }

    // runs before settings_load (SYS_INIT < main); values loaded from settings
    // are applied again by the commit handler
    apply_tap_hw();
    apply_tap_triggers();
    apply_carry_hw();

    return 0;
}

SYS_INIT(zmk_motion_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else /* !DT_HAS_CHOSEN(zmk_imu) */

bool zmk_motion_available(void) { return false; }

const char *zmk_motion_sensor_name(void) { return ""; }

int zmk_motion_get_tap_config(struct zmk_motion_tap_config *out) { return -ENOTSUP; }
int zmk_motion_set_tap_config(const struct zmk_motion_tap_config *cfg) { return -ENOTSUP; }
int zmk_motion_get_carry_config(struct zmk_motion_carry_config *out) { return -ENOTSUP; }
int zmk_motion_set_carry_config(const struct zmk_motion_carry_config *cfg) { return -ENOTSUP; }
int zmk_motion_get_still_wake_config(struct zmk_motion_still_wake_config *out) { return -ENOTSUP; }
int zmk_motion_set_still_wake_config(const struct zmk_motion_still_wake_config *cfg) {
    return -ENOTSUP;
}

int zmk_motion_save_state(void) { return -ENOTSUP; }
int zmk_motion_settings_reset(void) { return -ENOTSUP; }

int zmk_motion_set_live_stream(bool on) { return -ENOTSUP; }

#endif /* DT_HAS_CHOSEN(zmk_imu) */
