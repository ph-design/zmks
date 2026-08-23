/*
 * Copyright (c) 2025 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <hal/nrf_wdt.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/activity.h>
#include <zmk/endpoints.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zephyr/usb/usb_device.h>
#include <zmk/usb.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>

extern struct k_msgq zmk_hog_keyboard_msgq;
extern struct k_msgq zmk_hog_consumer_msgq;
#if IS_ENABLED(CONFIG_ZMK_POINTING)
extern struct k_msgq zmk_hog_mouse_msgq;
#endif
#endif

#define WDT_REG ((NRF_WDT_Type *)DT_REG_ADDR(DT_NODELABEL(wdt0)))

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel = -1;
// Armed when a transmission failed and no successful one followed; the window
// starts at the first failure and is cleared by the next success.
static atomic_t tx_stall_armed;
static volatile uint32_t tx_stall_start;
static volatile uint32_t tx_last_fail_at;
// Watermark refreshed whenever the BLE report queues are observed empty.
static int64_t tx_idle_since;

#define TX_FAIL_FRESH_MS 5000

static void wdt_feed_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(wdt_feed_work, wdt_feed_handler);

void zmk_wdt_feed_now(void) {
    if (wdt_channel >= 0) {
        wdt_feed(wdt, wdt_channel);
    }
}

void zmk_wdt_note_tx_ok(void) { atomic_set(&tx_stall_armed, 0); }

void zmk_wdt_note_tx_fail(void) {
    uint32_t now = k_uptime_get_32();
    if (!atomic_get(&tx_stall_armed) ||
        now - tx_stall_start > CONFIG_ZMK_UNRESPONSE_FIX_TX_STALL_MS) {
        tx_stall_start = now;
        atomic_set(&tx_stall_armed, 1);
    }
    tx_last_fail_at = now;
}

static bool transport_link_up(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB) {
        return zmk_usb_get_status() == USB_DC_CONFIGURED;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
    return zmk_ble_active_profile_is_connected();
#else
    return false;
#endif
}

static bool ble_selected_and_connected(void) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    return zmk_endpoints_selected().transport == ZMK_TRANSPORT_BLE &&
           zmk_ble_active_profile_is_connected();
#else
    return false;
#endif
}

#if IS_ENABLED(CONFIG_ZMK_BLE)
static bool hog_reports_pending(void) {
    if (k_msgq_num_used_get(&zmk_hog_keyboard_msgq) > 0 ||
        k_msgq_num_used_get(&zmk_hog_consumer_msgq) > 0) {
        return true;
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    return k_msgq_num_used_get(&zmk_hog_mouse_msgq) > 0;
#else
    return false;
#endif
}
#else
static bool hog_reports_pending(void) { return false; }
#endif

// Feeding
static void wdt_feed_handler(struct k_work *work) {
    int64_t now = k_uptime_get();

    if (!hog_reports_pending()) {
        tx_idle_since = now;
    }

    bool active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
    if (!active) {
        // Drop a stall armed before idle so it cannot fire after the idle boundary.
        atomic_set(&tx_stall_armed, 0);
    }

    // Only judge while the user is actually using the keyboard and a link exists.
    if (active && transport_link_up()) {
        uint32_t now32 = k_uptime_get_32();
        // Sends keep failing and none has gone out for the whole window.
        bool stalled = atomic_get(&tx_stall_armed) && now32 - tx_last_fail_at < TX_FAIL_FRESH_MS &&
                       now32 - tx_stall_start > CONFIG_ZMK_UNRESPONSE_FIX_TX_STALL_MS;
        // Reports are stuck in the BLE queues: the hog work queue is wedged.
        stalled = stalled || (ble_selected_and_connected() && hog_reports_pending() &&
                              now - tx_idle_since > CONFIG_ZMK_UNRESPONSE_FIX_TX_STALL_MS);

        if (stalled) {
            // Stop feeding and let the WDT reset us.
            LOG_ERR("HID transport stalled, stop feeding the watchdog");
            return;
        }
    }

    zmk_wdt_feed_now();
    k_work_schedule(&wdt_feed_work, K_SECONDS(1));
}

static int zmk_wdt_init(void) {
    if (!device_is_ready(wdt)) {
        return -ENODEV;
    }

    if (nrf_wdt_started_check(WDT_REG)) {
        // On nRF52 the WDT is not reset by a System OFF wake or warm reset and
        // its config registers are locked once started; just reload and keep feeding.
        wdt_channel = 0;
    } else {
        struct wdt_timeout_cfg cfg = {
            .window = {.min = 0, .max = 5000},
            .callback = NULL,
            .flags = WDT_FLAG_RESET_SOC,
        };
        wdt_channel = wdt_install_timeout(wdt, &cfg);
        if (wdt_channel < 0) {
            return wdt_channel;
        }

        // Pause while halted by debugger so breakpoints don't trigger a reset
        int err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
        if (err) {
            return err;
        }
    }

    // Feed immediately: nothing feeds the WDT between reset and this init during boot
    zmk_wdt_feed_now();
    tx_idle_since = k_uptime_get();
    k_work_schedule(&wdt_feed_work, K_SECONDS(1));
    return 0;
}

SYS_INIT(zmk_wdt_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
