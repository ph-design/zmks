/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/split_peripheral_layer_changed.h>
#include <zmk/events/sensor_event.h>

#include <zmk/pm.h>

#include <zmk/activity.h>
#include <zmk/watchdog.h>

#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
#include <zmk/matrix.h>
#endif

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING)
#include <zephyr/input/input.h>
#endif

bool is_usb_power_present(void) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    return zmk_usb_is_powered();
#else
    return false;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
}

static enum zmk_activity_state activity_state;

static atomic_t activity_last_uptime;

#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
static ATOMIC_DEFINE(held_positions, ZMK_KEYMAP_LEN);
static atomic_t held_count;
static atomic_t last_key;
static atomic_t has_key;

void zmk_activity_note_key(uint32_t position, bool pressed) {
    if (position < ZMK_KEYMAP_LEN) {
        if (pressed && !atomic_test_and_set_bit(held_positions, position)) {
            atomic_inc(&held_count);
        } else if (!pressed && atomic_test_and_clear_bit(held_positions, position)) {
            atomic_dec(&held_count);
        }
    }
    atomic_set(&last_key, k_uptime_get_32());
    atomic_set(&has_key, 1);
}

bool zmk_activity_input_recent(uint32_t quiet_ms) {
    return atomic_get(&held_count) > 0 ||
           (atomic_get(&has_key) && k_uptime_get_32() - (uint32_t)atomic_get(&last_key) < quiet_ms);
}
#endif

static void activity_work_handler(struct k_work *work);
static K_WORK_DEFINE(activity_work, activity_work_handler);

#define MAX_IDLE_MS CONFIG_ZMK_IDLE_TIMEOUT

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
#define MAX_SLEEP_MS CONFIG_ZMK_IDLE_SLEEP_TIMEOUT
#endif

int raise_event(void) {
    return raise_zmk_activity_state_changed(
        (struct zmk_activity_state_changed){.state = activity_state});
}

int set_state(enum zmk_activity_state state) {
    if (activity_state == state)
        return 0;

    activity_state = state;
    return raise_event();
}

enum zmk_activity_state zmk_activity_get_state(void) { return activity_state; }

static int note_activity(void) {
    atomic_set(&activity_last_uptime, k_uptime_get_32());

    return set_state(ZMK_ACTIVITY_ACTIVE);
}

void zmk_activity_note_activity(void) { note_activity(); }

static int activity_event_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_MOTION_HARDWARE_TAP)
    const struct zmk_position_state_changed *event = as_zmk_position_state_changed(eh);
    if (event && event->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        zmk_activity_note_key(event->position, event->state);
    }
#endif
    return note_activity();
}

static void activity_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t current = k_uptime_get_32();
    uint32_t inactive_time = current - (uint32_t)atomic_get(&activity_last_uptime);
    bool usb_powered = is_usb_power_present();
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    if (inactive_time > MAX_SLEEP_MS && !usb_powered) {
        set_state(ZMK_ACTIVITY_SLEEP);

        if (zmk_pm_suspend_devices() < 0) {
            LOG_ERR("Failed to suspend all the devices");
            zmk_pm_resume_devices();
            note_activity();
            return;
        }

        // Keep the WDT frozen with a full window across System OFF (see zmk_pm_soft_off)
        zmk_wdt_feed_now();

        sys_poweroff();
    }
#endif /* IS_ENABLED(CONFIG_ZMK_SLEEP) */
    if (inactive_time > MAX_IDLE_MS &&
        (!IS_ENABLED(CONFIG_ZMK_IDLE_SKIP_ON_USB_POWER) || !usb_powered)) {
        set_state(ZMK_ACTIVITY_IDLE);
    }
}

void zmk_activity_force_sleep(void) {
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    // backdate the idle clock so the work pass takes the normal sleep path
    atomic_set(&activity_last_uptime, k_uptime_get_32() - MAX_SLEEP_MS - MSEC_PER_SEC);
    k_work_submit(&activity_work);
#endif
}

void activity_expiry_function(struct k_timer *_timer) { k_work_submit(&activity_work); }

K_TIMER_DEFINE(activity_timer, activity_expiry_function, NULL);

static int activity_init(void) {
    atomic_set(&activity_last_uptime, k_uptime_get_32());

    k_timer_start(&activity_timer, K_SECONDS(1), K_SECONDS(1));
    return 0;
}

ZMK_LISTENER(activity, activity_event_listener);
ZMK_SUBSCRIPTION(activity, zmk_position_state_changed);
ZMK_SUBSCRIPTION(activity, zmk_sensor_event);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
ZMK_SUBSCRIPTION(activity, zmk_split_peripheral_layer_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING)

static void note_activity_work_cb(struct k_work *_work) { note_activity(); }

K_WORK_DEFINE(note_activity_work, note_activity_work_cb);

static void activity_input_listener(struct input_event *ev) { k_work_submit(&note_activity_work); }

INPUT_CALLBACK_DEFINE(NULL, activity_input_listener);

#endif

SYS_INIT(activity_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
