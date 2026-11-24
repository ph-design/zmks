/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zmk/activity.h>
#include <zmk/power.h>
#include <zmk/workqueue.h>
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
#include <zmk/backlight.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
#define DEFAULT_DEEP_SLEEP_MS CONFIG_ZMK_POWER_DEEP_SLEEP_TIMEOUT_MS
#else
#define DEFAULT_DEEP_SLEEP_MS 0
#endif

#define POWER_DEFAULTS                                                                             \
    {.idle_timeout_ms = CONFIG_ZMK_POWER_IDLE_TIMEOUT_MS,                                          \
     .rgb_off_ms = 30000,                                                                          \
     .backlight_off_ms = 30000,                                                                    \
     .deep_sleep_ms = DEFAULT_DEEP_SLEEP_MS,                                                       \
     .on_usb = IS_ENABLED(CONFIG_ZMK_POWER_IDLE_ON_USB),                                          \
     .off_when_locked = true}

enum light { RGB_IDLE, BACKLIGHT_OFF, LIGHTS_LOCKED };

enum power_flag {
    POWER_ON_USB = BIT(0),
    POWER_OFF_WHEN_LOCKED = BIT(1),
    POWER_RGB_USB_OVERRIDE = BIT(2),
    POWER_RGB_ON_USB = BIT(3),
};

static struct zmk_power_config config = POWER_DEFAULTS;
static struct zmk_power_config saved_config = POWER_DEFAULTS;
static struct k_spinlock config_lock;
static atomic_t suppressed;

static void refresh_backlight(struct k_work *work) {
    ARG_UNUSED(work);
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
    zmk_backlight_update();
#endif
}

static K_WORK_DEFINE(backlight_work, refresh_backlight);

void zmk_power_get_config(struct zmk_power_config *out) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    *out = config;
    k_spin_unlock(&config_lock, key);
}

static bool valid_timer(uint32_t ms, uint32_t minimum) {
    return !ms || (ms >= minimum && ms <= 24 * 60 * 60 * MSEC_PER_SEC && ms % MSEC_PER_SEC == 0);
}

static bool valid_config(const struct zmk_power_config *cfg) {
    uint32_t deep = cfg->deep_sleep_ms;
    return valid_timer(cfg->idle_timeout_ms, 1000) && valid_timer(cfg->rgb_off_ms, 1000) &&
           valid_timer(cfg->backlight_off_ms, 1000) && valid_timer(deep, 60000) &&
           (!deep || (IS_ENABLED(CONFIG_ZMK_SLEEP) && deep >= cfg->idle_timeout_ms &&
                      deep >= cfg->rgb_off_ms && deep >= cfg->backlight_off_ms));
}

static void apply_config(const struct zmk_power_config *cfg) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    config = *cfg;
    k_spin_unlock(&config_lock, key);
    zmk_activity_note_activity();
}

int zmk_power_set_config(const struct zmk_power_config *cfg) {
    if (!valid_config(cfg)) {
        return -EINVAL;
    }
    apply_config(cfg);
    return 0;
}

struct power_record {
    uint32_t version;
    uint32_t idle;
    uint32_t rgb;
    uint32_t backlight;
    uint32_t deep;
    uint32_t flags;
};

static void set_saved_config(const struct zmk_power_config *cfg) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    saved_config = *cfg;
    k_spin_unlock(&config_lock, key);
}

static int save_config(const struct zmk_power_config *cfg) {
    struct power_record record = {
        .version = 1,
        .idle = cfg->idle_timeout_ms,
        .rgb = cfg->rgb_off_ms,
        .backlight = cfg->backlight_off_ms,
        .deep = cfg->deep_sleep_ms,
        .flags = (cfg->on_usb ? POWER_ON_USB : 0) |
                 (cfg->off_when_locked ? POWER_OFF_WHEN_LOCKED : 0) |
                 (cfg->rgb_usb_override ? POWER_RGB_USB_OVERRIDE : 0) |
                 (cfg->rgb_on_usb ? POWER_RGB_ON_USB : 0),
    };
    int err = settings_save_one("power/v1", &record, sizeof(record));
    if (!err) {
        set_saved_config(cfg);
    }
    return err;
}

int zmk_power_save_config(void) {
    struct zmk_power_config cfg;
    zmk_power_get_config(&cfg);
    return save_config(&cfg);
}

bool zmk_power_rgb_on_usb(const struct zmk_power_config *cfg) {
    return cfg->rgb_usb_override ? cfg->rgb_on_usb : cfg->on_usb;
}

static void set_rgb_idle(struct zmk_power_config *cfg, uint32_t timeout_ms, bool on_usb) {
    cfg->rgb_off_ms = timeout_ms;
    cfg->rgb_usb_override = true;
    cfg->rgb_on_usb = on_usb;
}

static struct zmk_power_config get_saved_config(void) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    struct zmk_power_config stored = saved_config;
    k_spin_unlock(&config_lock, key);
    return stored;
}

static bool rgb_idle_equal(const struct zmk_power_config *a, const struct zmk_power_config *b) {
    return a->rgb_off_ms == b->rgb_off_ms && a->rgb_usb_override == b->rgb_usb_override &&
           a->rgb_on_usb == b->rgb_on_usb;
}

static void copy_rgb_idle(struct zmk_power_config *to, const struct zmk_power_config *from) {
    to->rgb_off_ms = from->rgb_off_ms;
    to->rgb_usb_override = from->rgb_usb_override;
    to->rgb_on_usb = from->rgb_on_usb;
}

static int save_rgb_idle(const struct zmk_power_config *cfg) {
    struct zmk_power_config stored = get_saved_config();
    if (rgb_idle_equal(&stored, cfg)) {
        return 0;
    }
    copy_rgb_idle(&stored, cfg);
    return valid_config(&stored) ? save_config(&stored) : -EINVAL;
}

int zmk_power_set_rgb_idle(uint32_t timeout_ms, bool on_usb) {
    struct zmk_power_config cfg;
    zmk_power_get_config(&cfg);
    set_rgb_idle(&cfg, timeout_ms, on_usb);
    if (!valid_config(&cfg)) {
        return -EINVAL;
    }
    apply_config(&cfg);
    return 0;
}

bool zmk_power_rgb_idle_changed(void) {
    struct zmk_power_config cfg;
    zmk_power_get_config(&cfg);
    struct zmk_power_config stored = get_saved_config();
    return !rgb_idle_equal(&cfg, &stored);
}

int zmk_power_save_rgb_idle(void) {
    struct zmk_power_config cfg;
    zmk_power_get_config(&cfg);
    return save_rgb_idle(&cfg);
}

void zmk_power_discard_rgb_idle(void) {
    struct zmk_power_config cfg;
    zmk_power_get_config(&cfg);
    struct zmk_power_config stored = get_saved_config();
    if (!rgb_idle_equal(&cfg, &stored)) {
        copy_rgb_idle(&cfg, &stored);
        apply_config(&cfg);
    }
}

int zmk_power_reset_config(void) {
    int err = settings_delete("power/v1");
    if (err < 0 && err != -ENOENT) {
        return err;
    }
    const struct zmk_power_config defaults = POWER_DEFAULTS;
    set_saved_config(&defaults);
    apply_config(&defaults);
    return 0;
}

static int power_load(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (strcmp(name, "v1")) {
        return -ENOENT;
    }
    struct power_record record;
    if (len != sizeof(record) || read(arg, &record, len) != sizeof(record) || record.version != 1 ||
        record.flags & ~(POWER_ON_USB | POWER_OFF_WHEN_LOCKED | POWER_RGB_USB_OVERRIDE |
                         POWER_RGB_ON_USB)) {
        return -EINVAL;
    }
    const struct zmk_power_config cfg = {
        .idle_timeout_ms = record.idle,
        .rgb_off_ms = record.rgb,
        .backlight_off_ms = record.backlight,
        .deep_sleep_ms = record.deep,
        .on_usb = record.flags & POWER_ON_USB,
        .off_when_locked = record.flags & POWER_OFF_WHEN_LOCKED,
        .rgb_usb_override = record.flags & POWER_RGB_USB_OVERRIDE,
        .rgb_on_usb = record.flags & POWER_RGB_ON_USB,
    };
    if (!valid_config(&cfg)) {
        return -EINVAL;
    }
    set_saved_config(&cfg);
    apply_config(&cfg);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_power, "power", NULL, power_load, NULL, NULL);

static bool elapsed(uint32_t age, uint32_t timer) { return timer && age >= timer; }

struct zmk_power_state zmk_power_update(uint32_t inactive_ms, bool held, bool usb_powered,
                                        bool locked, bool studio_unlocked) {
    struct zmk_power_config cfg;
    zmk_power_get_config(&cfg);
    bool timed = !held && (!usb_powered || cfg.on_usb);
    bool lock_off = locked && cfg.off_when_locked;
    bool rgb_timed = !held && (!usb_powered || zmk_power_rgb_on_usb(&cfg));
    bool rgb_idle = rgb_timed && elapsed(inactive_ms, cfg.rgb_off_ms);
    bool backlight_off = lock_off || (timed && elapsed(inactive_ms, cfg.backlight_off_ms));
    atomic_val_t next = (rgb_idle ? BIT(RGB_IDLE) : 0) | (backlight_off ? BIT(BACKLIGHT_OFF) : 0) |
                        (lock_off ? BIT(LIGHTS_LOCKED) : 0);
    atomic_val_t changed = atomic_set(&suppressed, next) ^ next;
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    if (changed & (BIT(RGB_IDLE) | BIT(LIGHTS_LOCKED))) {
        zmk_rgb_underglow_apply_state();
    }
#endif
    if (IS_ENABLED(CONFIG_ZMK_BACKLIGHT) && (changed & BIT(BACKLIGHT_OFF))) {
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &backlight_work);
    }
    return (struct zmk_power_state){
        .idle = locked || (timed && elapsed(inactive_ms, cfg.idle_timeout_ms)),
        .deep_sleep = !usb_powered && !held && !locked && !studio_unlocked &&
                      elapsed(inactive_ms, cfg.deep_sleep_ms),
    };
}

bool zmk_power_rgb_off(void) {
    return (atomic_get(&suppressed) & (BIT(RGB_IDLE) | BIT(LIGHTS_LOCKED))) != 0;
}

bool zmk_power_lights_locked(void) { return atomic_test_bit(&suppressed, LIGHTS_LOCKED); }

bool zmk_power_backlight_off(void) { return atomic_test_bit(&suppressed, BACKLIGHT_OFF); }
