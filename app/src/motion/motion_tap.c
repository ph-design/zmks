/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <zephyr/drivers/sensor/lis2dh.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zmk/activity.h>
#include <zmk/keymap.h>
#include <zmk/motion_guard.h>
#include <zmk/motion_sample.h>
#include <zmk/motion_tap.h>
#include <zmk/virtual_key_position.h>

#define CLICK_DOUBLE BIT(5)
#define CLICK_SINGLE BIT(4)
#define CLICK_NEGATIVE BIT(3)
#define TAP_KEY_VETO_MS 12
#define TAP_DEFAULTS {.axes = 7, .quiet_ms = 120}

static const struct device *const imu = DEVICE_DT_GET(DT_CHOSEN(zmk_imu));
static struct zmk_tap_options options = TAP_DEFAULTS;
static struct zmk_motion_tap_config config;
static struct zmk_tap_diagnostics diagnostics;
static struct zmk_tap_capture capture;
static uint8_t samples[ZMK_TAP_CAPTURE_FRAMES][ZMK_TAP_FRAME_SIZE];
static uint8_t next_sample, sample_count;
static uint32_t generation;
static int64_t test_until;
static bool pending_click;
static struct k_spinlock tap_lock;
static K_MUTEX_DEFINE(tap_config_lock);

struct tap_event {
    uint32_t generation;
    uint32_t guard_generation;
    int64_t timestamp;
    uint8_t src;
    bool test;
};

static struct tap_event click_edge;
K_MSGQ_DEFINE(tap_events, sizeof(struct tap_event), 8, 4);

static uint32_t guard_generation(void) {
    return IS_ENABLED(CONFIG_ZMK_MOTION_GUARD) ? zmk_motion_guard_generation() : 0;
}

static void dispatch_tap(struct tap_event event) {
    if (k_mutex_lock(&tap_config_lock, K_NO_WAIT)) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    struct zmk_motion_tap_config cfg = config;
    struct zmk_tap_options opts = options;
    bool eligible = event.generation == generation && cfg.enabled;
    bool dry = event.test || k_uptime_get() < test_until;
    k_spin_unlock(&tap_lock, key);

    if (!eligible || event.guard_generation != guard_generation() ||
        (IS_ENABLED(CONFIG_ZMK_MOTION_GUARD) && zmk_motion_guard_locked()) ||
        zmk_activity_input_recent(opts.quiet_ms) ||
        (cfg.layer_mask && !(zmk_keymap_layer_state() & cfg.layer_mask))) {
        key = k_spin_lock(&tap_lock);
        diagnostics.suppressed++;
        k_spin_unlock(&tap_lock, key);
        k_mutex_unlock(&tap_config_lock);
        return;
    }

    bool negative = (event.src & CLICK_NEGATIVE) != 0;
    bool right = opts.split && (negative == IS_ENABLED(CONFIG_ZMK_MOTION_TAP_SIDE_INVERT));
    const struct zmk_behavior_binding *binding = right ? &cfg.right_double : &cfg.left_double;
    bool invoke = !dry && binding->behavior_dev;
    key = k_spin_lock(&tap_lock);
    diagnostics.gestures++;
    pending_click = true;
    k_spin_unlock(&tap_lock, key);
    if (!invoke) {
        k_mutex_unlock(&tap_config_lock);
        return;
    }

    struct zmk_behavior_binding_event binding_event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_MOTION(right),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    zmk_activity_note_activity();
    zmk_behavior_invoke_binding(binding, binding_event, true);
    zmk_behavior_invoke_binding(binding, binding_event, false);
    key = k_spin_lock(&tap_lock);
    diagnostics.actions++;
    k_spin_unlock(&tap_lock, key);
    k_mutex_unlock(&tap_config_lock);
}

static void tap_work_handler(struct k_work *work) {
    struct tap_event event;
    for (;;) {
        k_spinlock_key_t key = k_spin_lock(&tap_lock);
        if (k_msgq_peek(&tap_events, &event)) {
            k_spin_unlock(&tap_lock, key);
            return;
        }
        int64_t elapsed = k_uptime_get() - event.timestamp;
        if (elapsed < TAP_KEY_VETO_MS) {
            k_work_schedule(k_work_delayable_from_work(work), K_MSEC(TAP_KEY_VETO_MS - elapsed));
            k_spin_unlock(&tap_lock, key);
            return;
        }
        k_msgq_get(&tap_events, &event, K_NO_WAIT);
        k_spin_unlock(&tap_lock, key);
        dispatch_tap(event);
    }
}

static K_WORK_DELAYABLE_DEFINE(tap_work, tap_work_handler);

static void tap_trigger(const struct device *dev, const struct sensor_trigger *trigger) {
    ARG_UNUSED(trigger);
    struct sensor_value src;
    if (sensor_channel_get(dev, SENSOR_CHAN_LIS2DH_CLICK_SRC, &src) || !(src.val1 & CLICK_DOUBLE)) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    struct tap_event event = click_edge;
    event.src = src.val1;
    if (k_msgq_put(&tap_events, &event, K_NO_WAIT)) {
        diagnostics.drops++;
        generation++;
    } else {
        k_work_schedule(&tap_work, K_MSEC(TAP_KEY_VETO_MS));
    }
    k_spin_unlock(&tap_lock, key);
}

static int apply_hardware(const struct zmk_motion_tap_config *cfg,
                          const struct zmk_tap_options *opts) {
    static const struct sensor_trigger double_click = {.type = SENSOR_TRIG_DOUBLE_TAP,
                                                       .chan = SENSOR_CHAN_ACCEL_XYZ};
    const struct {
        enum sensor_attribute attribute;
        int32_t value;
    } attrs[] = {
        {SENSOR_ATTR_SAMPLING_FREQUENCY, cfg->enabled ? 400 : 100},
        {SENSOR_ATTR_LIS2DH_CLICK_THS, MAX(1, cfg->threshold / 2)},
        {SENSOR_ATTR_LIS2DH_CLICK_TIME_LIMIT_MS, cfg->time_limit_ms},
        {SENSOR_ATTR_LIS2DH_CLICK_LATENCY_MS, cfg->latency_ms},
        {SENSOR_ATTR_LIS2DH_CLICK_WINDOW_MS, cfg->window_ms},
    };
    for (size_t i = 0; i < ARRAY_SIZE(attrs); i++) {
        const struct sensor_value value = {.val1 = attrs[i].value};
        int err = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, attrs[i].attribute, &value);
        if (err) {
            return err;
        }
    }
    zmk_lis2dh_set_double_click_axes(imu, opts->axes);
    return sensor_trigger_set(imu, &double_click, cfg->enabled ? tap_trigger : NULL);
}

void zmk_tap_get_options(struct zmk_tap_options *out) {
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    *out = options;
    k_spin_unlock(&tap_lock, key);
}

static bool valid_options(const struct zmk_tap_options *opts) {
    return opts->axes >= 1 && opts->axes <= 7 && opts->quiet_ms <= 1000;
}

int zmk_tap_configure(const struct zmk_motion_tap_config *cfg, const struct zmk_tap_options *opts) {
    if (!valid_options(opts) || cfg->threshold > 127 || cfg->time_limit_ms < 1 ||
        cfg->time_limit_ms > 317 || cfg->latency_ms < 1 || cfg->latency_ms > 637 ||
        cfg->window_ms < 1 || cfg->window_ms > 637) {
        return -EINVAL;
    }
    if (!device_is_ready(imu)) {
        return -ENODEV;
    }
    k_mutex_lock(&tap_config_lock, K_FOREVER);
    zmk_motion_sample_lock();
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    struct zmk_motion_tap_config previous = config;
    struct zmk_tap_options previous_options = options;
    config.enabled = false;
    generation++;
    k_msgq_purge(&tap_events);
    k_spin_unlock(&tap_lock, key);

    int err = apply_hardware(cfg, opts);
    if (err && previous.time_limit_ms) {
        if (apply_hardware(&previous, &previous_options)) {
            previous.enabled = false;
        }
    }
    key = k_spin_lock(&tap_lock);
    config = err ? previous : *cfg;
    if (!err) {
        options = *opts;
    }
    generation++;
    if (!config.enabled) {
        test_until = 0;
    }
    diagnostics.error = err;
    diagnostics.odr = config.enabled ? 400 : 100;
    diagnostics.engine = config.enabled ? 2 : 0;
    k_spin_unlock(&tap_lock, key);
    zmk_motion_sample_unlock();
    k_mutex_unlock(&tap_config_lock);
    return err;
}

int zmk_tap_set_test(bool on) {
    k_mutex_lock(&tap_config_lock, K_FOREVER);
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    if (on && !config.enabled) {
        k_spin_unlock(&tap_lock, key);
        k_mutex_unlock(&tap_config_lock);
        return -EACCES;
    }
    if (on) {
        diagnostics.irq = diagnostics.single = diagnostics.dbl = 0;
        diagnostics.gestures = diagnostics.actions = diagnostics.suppressed = diagnostics.drops = 0;
        memset(diagnostics.axes, 0, sizeof(diagnostics.axes));
        capture.count = sample_count = 0;
    }
    generation++;
    k_msgq_purge(&tap_events);
    test_until = on ? k_uptime_get() + 60000 : 0;
    k_spin_unlock(&tap_lock, key);
    k_mutex_unlock(&tap_config_lock);
    return 0;
}

bool zmk_tap_test_active(void) {
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    bool active = k_uptime_get() < test_until;
    k_spin_unlock(&tap_lock, key);
    return active;
}

void zmk_tap_get_diagnostics(struct zmk_tap_diagnostics *out) {
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    *out = diagnostics;
    out->test_ms = MAX(0, test_until - k_uptime_get());
    k_spin_unlock(&tap_lock, key);

    if (!device_is_ready(imu)) {
        out->error = -ENODEV;
        return;
    }
    zmk_motion_sample_lock();
    int err = zmk_lis2dh_click_snapshot(imu, out->registers, &out->full_scale_mg);
    zmk_motion_sample_unlock();
    if (err && !out->error) {
        out->error = err;
    }
}

void zmk_tap_get_capture(struct zmk_tap_capture *out) {
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    *out = capture;
    k_spin_unlock(&tap_lock, key);
}

void zmk_tap_sample(const struct sensor_value xyz[3], int64_t timestamp) {
    int32_t mg[3];
    uint8_t frame[ZMK_TAP_FRAME_SIZE];
    sys_put_le16((uint16_t)timestamp, frame);
    for (size_t i = 0; i < ARRAY_SIZE(mg); i++) {
        mg[i] = zmk_motion_sample_mg(&xyz[i]);
        sys_put_le16((int16_t)CLAMP(mg[i], INT16_MIN, INT16_MAX), &frame[2 + 2 * i]);
    }
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    if (config.enabled) {
        memcpy(diagnostics.current, mg, sizeof(mg));
        memcpy(samples[next_sample], frame, sizeof(frame));
        next_sample = (next_sample + 1) % ARRAY_SIZE(samples);
        if (sample_count < ARRAY_SIZE(samples)) {
            sample_count++;
        }
    }
    k_spin_unlock(&tap_lock, key);
}

void zmk_tap_gpio(void) {
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    int64_t now = k_uptime_get();
    click_edge = (struct tap_event){
        .generation = generation,
        .guard_generation = guard_generation(),
        .timestamp = now,
        .test = now < test_until,
    };
    diagnostics.irq++;
    k_spin_unlock(&tap_lock, key);
}

void zmk_tap_click(uint8_t src) {
    if (!(src & (CLICK_SINGLE | CLICK_DOUBLE))) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    diagnostics.src = src;
    diagnostics.single += !!(src & CLICK_SINGLE);
    diagnostics.dbl += !!(src & CLICK_DOUBLE);
    for (size_t i = 0; i < ARRAY_SIZE(diagnostics.axes); i++) {
        diagnostics.axes[i] += !!(src & BIT(i));
    }
    capture.sequence++;
    capture.count = sample_count;
    size_t first = next_sample + ARRAY_SIZE(samples) - sample_count;
    for (size_t i = 0; i < sample_count; i++) {
        memcpy(&capture.frames[i * ZMK_TAP_FRAME_SIZE], samples[(first + i) % ARRAY_SIZE(samples)],
               ZMK_TAP_FRAME_SIZE);
    }
    k_spin_unlock(&tap_lock, key);
}

bool zmk_tap_take_click(uint8_t *src) {
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    bool pending = pending_click;
    *src = diagnostics.src;
    pending_click = false;
    k_spin_unlock(&tap_lock, key);
    return pending;
}

struct tap_record {
    uint32_t version, axes, quiet, split;
};

int zmk_tap_save(void) {
    struct zmk_tap_options opts;
    zmk_tap_get_options(&opts);
    const struct tap_record record = {1, opts.axes, opts.quiet_ms, opts.split};
    return settings_save_one("tap/v1", &record, sizeof(record));
}

int zmk_tap_reset(void) {
    int err = settings_delete("tap/v1");
    if (err < 0 && err != -ENOENT) {
        return err;
    }
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    options = (struct zmk_tap_options)TAP_DEFAULTS;
    k_spin_unlock(&tap_lock, key);
    return zmk_tap_set_test(false);
}

static int tap_load(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (strcmp(name, "v1")) {
        return -ENOENT;
    }
    struct tap_record record;
    if (len != sizeof(record) || read(arg, &record, len) != sizeof(record) || record.version != 1 ||
        record.split > 1) {
        return -EINVAL;
    }
    const struct zmk_tap_options loaded = {record.axes, record.quiet, record.split};
    if (!valid_options(&loaded)) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&tap_lock);
    options = loaded;
    k_spin_unlock(&tap_lock, key);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_tap, "tap", NULL, tap_load, NULL, NULL);
