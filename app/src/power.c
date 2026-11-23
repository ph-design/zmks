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

enum light { RGB_OFF, BACKLIGHT_OFF };

static struct zmk_power_config config = POWER_DEFAULTS;
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

int zmk_power_set_config(const struct zmk_power_config *requested) {
    uint32_t deep = requested->deep_sleep_ms;
    if (!valid_timer(requested->idle_timeout_ms, 1000) ||
        !valid_timer(requested->rgb_off_ms, 1000) ||
        !valid_timer(requested->backlight_off_ms, 1000) || !valid_timer(deep, 60000) ||
        (deep && (!IS_ENABLED(CONFIG_ZMK_SLEEP) || deep < requested->idle_timeout_ms ||
                  deep < requested->rgb_off_ms || deep < requested->backlight_off_ms))) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&config_lock);
    config = *requested;
    k_spin_unlock(&config_lock, key);
    zmk_activity_note_activity();
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

int zmk_power_save_config(void) {
    struct zmk_power_config cfg;
    zmk_power_get_config(&cfg);
    struct power_record record = {
        .version = 1,
        .idle = cfg.idle_timeout_ms,
        .rgb = cfg.rgb_off_ms,
        .backlight = cfg.backlight_off_ms,
        .deep = cfg.deep_sleep_ms,
        .flags = (cfg.on_usb ? BIT(0) : 0) | (cfg.off_when_locked ? BIT(1) : 0),
    };
    return settings_save_one("power/v1", &record, sizeof(record));
}

int zmk_power_reset_config(void) {
    int err = settings_delete("power/v1");
    if (err < 0 && err != -ENOENT) {
        return err;
    }
    const struct zmk_power_config defaults = POWER_DEFAULTS;
    return zmk_power_set_config(&defaults);
}

static int power_load(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (strcmp(name, "v1")) {
        return -ENOENT;
    }
    struct power_record record;
    if (len != sizeof(record) || read(arg, &record, len) != sizeof(record) || record.version != 1 ||
        record.flags & ~(BIT(0) | BIT(1))) {
        return -EINVAL;
    }
    const struct zmk_power_config cfg = {
        .idle_timeout_ms = record.idle,
        .rgb_off_ms = record.rgb,
        .backlight_off_ms = record.backlight,
        .deep_sleep_ms = record.deep,
        .on_usb = record.flags & BIT(0),
        .off_when_locked = record.flags & BIT(1),
    };
    return zmk_power_set_config(&cfg);
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_power, "power", NULL, power_load, NULL, NULL);

static bool elapsed(uint32_t age, uint32_t timer) { return timer && age >= timer; }

struct zmk_power_state zmk_power_update(uint32_t inactive_ms, bool held, bool usb_powered,
                                        bool locked, bool studio_unlocked) {
    struct zmk_power_config cfg;
    zmk_power_get_config(&cfg);
    bool timed = !held && (!usb_powered || cfg.on_usb);
    bool lock_off = locked && cfg.off_when_locked;
    bool rgb_off = lock_off || (timed && elapsed(inactive_ms, cfg.rgb_off_ms));
    bool backlight_off = lock_off || (timed && elapsed(inactive_ms, cfg.backlight_off_ms));
    atomic_val_t next = (rgb_off ? BIT(RGB_OFF) : 0) | (backlight_off ? BIT(BACKLIGHT_OFF) : 0);
    atomic_val_t changed = atomic_set(&suppressed, next) ^ next;
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    if (changed & BIT(RGB_OFF)) {
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

bool zmk_power_rgb_off(void) { return atomic_test_bit(&suppressed, RGB_OFF); }

bool zmk_power_backlight_off(void) { return atomic_test_bit(&suppressed, BACKLIGHT_OFF); }
