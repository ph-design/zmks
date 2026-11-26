/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/pm/pm.h>
#include <zephyr/sys/poweroff.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/endpoints.h>
#include <zmk/motion_sample.h>
#include <zmk/rgb_underglow.h>
#include <zmk/watchdog.h>

// Reimplement some of the device work from Zephyr PM to work with the new `sys_poweroff` API.
// TODO: Tweak this to smarter runtime PM of subsystems on sleep.

#ifdef CONFIG_ZMK_PM_DEVICE_SUSPEND_RESUME
TYPE_SECTION_START_EXTERN(const struct device *, pm_device_slots);

#if !defined(CONFIG_PM_DEVICE_RUNTIME_EXCLUSIVE)
/* Number of devices successfully suspended. */
static size_t zmk_num_susp;

int zmk_pm_suspend_devices(void) {
    const struct device *devs;
    size_t devc;

    devc = z_device_get_all_static(&devs);

    zmk_num_susp = 0;
#if IS_ENABLED(CONFIG_ZMK_MOTION_SAMPLER)
    int sample_err = zmk_motion_sample_suspend();
    if (sample_err < 0) {
        return sample_err;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    int err = zmk_rgb_underglow_suspend();
    if (err < 0) {
        return err;
    }
#endif

    for (const struct device *dev = devs + devc - 1; dev >= devs; dev--) {
        int ret;

        /*
         * Ignore uninitialized devices, busy devices, wake up sources, and
         * devices with runtime PM enabled.
         */
        if (!device_is_ready(dev) || pm_device_is_busy(dev) || pm_device_state_is_locked(dev) ||
            pm_device_wakeup_is_enabled(dev) || pm_device_runtime_is_enabled(dev)) {
            continue;
        }

        ret = pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
        /* ignore devices not supporting or already at the given state */
        if ((ret == -ENOSYS) || (ret == -ENOTSUP) || (ret == -EALREADY)) {
            continue;
        } else if (ret < 0) {
            LOG_ERR("Device %s did not enter %s state (%d)", dev->name,
                    pm_device_state_str(PM_DEVICE_STATE_SUSPENDED), ret);
            return ret;
        }

        TYPE_SECTION_START(pm_device_slots)[zmk_num_susp] = dev;
        zmk_num_susp++;
    }

    return 0;
}

void zmk_pm_resume_devices(void) {
    for (int i = (zmk_num_susp - 1); i >= 0; i--) {
        pm_device_action_run(TYPE_SECTION_START(pm_device_slots)[i], PM_DEVICE_ACTION_RESUME);
    }

    zmk_num_susp = 0;
#if IS_ENABLED(CONFIG_ZMK_MOTION_SAMPLER)
    zmk_motion_sample_resume();
#endif
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    zmk_rgb_underglow_resume();
#endif
}
#endif /* !CONFIG_PM_DEVICE_RUNTIME_EXCLUSIVE */
#endif /* CONFIG_ZMK_PM_DEVICE_SUSPEND_RESUME */

#if IS_ENABLED(CONFIG_ZMK_PM_SOFT_OFF)

#define HAS_WAKERS DT_HAS_COMPAT_STATUS_OKAY(zmk_soft_off_wakeup_sources)

#if HAS_WAKERS

#define DEVICE_WITH_SEP(node_id, prop, idx) DEVICE_DT_GET(DT_PROP_BY_IDX(node_id, prop, idx)),

const struct device *soft_off_wakeup_sources[] = {
    DT_FOREACH_PROP_ELEM(DT_INST(0, zmk_soft_off_wakeup_sources), wakeup_sources, DEVICE_WITH_SEP)};

#endif

#if IS_ENABLED(CONFIG_PM_DEVICE)
static bool soft_off_wakeup_source(const struct device *dev) {
#if HAS_WAKERS
    for (size_t i = 0; i < ARRAY_SIZE(soft_off_wakeup_sources); i++) {
        if (soft_off_wakeup_sources[i] == dev) {
            return true;
        }
    }
#else
    ARG_UNUSED(dev);
#endif
    return false;
}
#endif

int zmk_pm_soft_off(void) {
    // The WDT keeps its remaining timeout frozen across System OFF on nRF52 and
    // resumes counting during boot after wake, so reload it before going off.
    zmk_wdt_feed_now();

#if IS_ENABLED(CONFIG_PM_DEVICE)
    size_t device_count;
    const struct device *devs;

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zmk_endpoints_clear_current();
    // Need to sleep to give any other threads a chance so submit endpoint data.
    k_sleep(K_MSEC(100));
#endif

    device_count = z_device_get_all_static(&devs);
    bool wakeup_enabled[device_count];

    for (size_t i = 0; i < device_count; i++) {
        const struct device *dev = &devs[i];
        wakeup_enabled[i] = pm_device_wakeup_is_enabled(dev);
        bool wake = soft_off_wakeup_source(dev);
        if (wake != wakeup_enabled[i]) {
            pm_device_wakeup_enable(dev, wake);
        }
    }
#endif // IS_ENABLED(CONFIG_PM_DEVICE)

    int err = zmk_pm_suspend_devices();
    if (err < 0) {
        goto resume_devices;
    }

#if HAS_WAKERS
    // Wakeup sources can share GPIOs with the suspended devices.
    const struct device *resumed_wakers[ARRAY_SIZE(soft_off_wakeup_sources)];
    size_t resumed = 0;

    for (size_t i = 0; i < ARRAY_SIZE(soft_off_wakeup_sources); i++) {
        const struct device *dev = soft_off_wakeup_sources[i];
        err = pm_device_action_run(dev, PM_DEVICE_ACTION_RESUME);
        if (err == 0) {
            resumed_wakers[resumed++] = dev;
        } else if (err != -EALREADY && err != -ENOSYS && err != -ENOTSUP) {
            while (resumed > 0) {
                pm_device_action_run(resumed_wakers[--resumed], PM_DEVICE_ACTION_SUSPEND);
            }
            goto resume_devices;
        }
    }
#endif // HAS_WAKERS

    LOG_DBG("soft-off: go to sleep");
    zmk_wdt_feed_now();
    sys_poweroff();
    return 0;

resume_devices:
    zmk_pm_resume_devices();
#if IS_ENABLED(CONFIG_PM_DEVICE)
    for (size_t i = 0; i < device_count; i++) {
        if (wakeup_enabled[i] != pm_device_wakeup_is_enabled(&devs[i])) {
            pm_device_wakeup_enable(&devs[i], wakeup_enabled[i]);
        }
    }
#endif
    return err;
}

#endif // IS_ENABLED(CONFIG_ZMK_PM_SOFT_OFF)
