/*
 * Copyright (c) 2025 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel;

static void wdt_feed_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(wdt_feed_work, wdt_feed_handler);

// Feeding
static void wdt_feed_handler(struct k_work *work) {
    wdt_feed(wdt, wdt_channel);
    k_work_schedule(&wdt_feed_work, K_SECONDS(1));
}

static int zmk_wdt_init(void) {
    if (!device_is_ready(wdt)) {
        return -ENODEV;
    }

    struct wdt_timeout_cfg cfg = {
        .window = {.min = 0, .max = 3000},
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

    k_work_schedule(&wdt_feed_work, K_SECONDS(1));
    return 0;
}

SYS_INIT(zmk_wdt_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
