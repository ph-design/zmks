/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/types.h>
#include <stddef.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @cond INTERNAL_HIDDEN
 *
 * Behavior driver API definition and system call entry points.
 *
 * (Internal use only.)
 */

typedef int (*ext_power_enable_t)(const struct device *dev);
typedef int (*ext_power_disable_t)(const struct device *dev);
typedef int (*ext_power_get_t)(const struct device *dev);

__subsystem struct ext_power_api {
    ext_power_enable_t enable;
    ext_power_disable_t disable;
    ext_power_get_t get;
    ext_power_enable_t enable_transient;
    ext_power_disable_t disable_transient;
};
/**
 * @endcond
 */

/**
 * @brief Enable/disable without saving the state
 */
static inline int ext_power_enable_transient(const struct device *dev) {
    const struct ext_power_api *api = (const struct ext_power_api *)dev->api;

    if (api->enable_transient == NULL) {
        return -ENOTSUP;
    }

    return api->enable_transient(dev);
}

static inline int ext_power_disable_transient(const struct device *dev) {
    const struct ext_power_api *api = (const struct ext_power_api *)dev->api;

    if (api->disable_transient == NULL) {
        return -ENOTSUP;
    }

    return api->disable_transient(dev);
}

/**
 * @brief Enable the external power output
 * @param dev Pointer to the device structure for the driver instance.
 *
 * @retval 0 If successful.
 * @retval Negative errno code if failure.
 */
__syscall int ext_power_enable(const struct device *dev);

static inline int z_impl_ext_power_enable(const struct device *dev) {
    const struct ext_power_api *api = (const struct ext_power_api *)dev->api;

    if (api->enable == NULL) {
        return -ENOTSUP;
    }

    return api->enable(dev);
}

/**
 * @brief Disable the external power output
 * @param dev Pointer to the device structure for the driver instance.
 *
 * @retval 0 If successful.
 * @retval Negative errno code if failure.
 */
__syscall int ext_power_disable(const struct device *dev);

static inline int z_impl_ext_power_disable(const struct device *dev) {
    const struct ext_power_api *api = (const struct ext_power_api *)dev->api;

    if (api->disable == NULL) {
        return -ENOTSUP;
    }

    return api->disable(dev);
}

/**
 * @brief Get the current status of the external power output
 * @param dev Pointer to the device structure for the driver instance.
 *
 * @retval 0 If ext power is disabled.
 * @retval 1 if ext power is enabled.
 * @retval Negative errno code if failure.
 */
__syscall int ext_power_get(const struct device *dev);

static inline int z_impl_ext_power_get(const struct device *dev) {
    const struct ext_power_api *api = (const struct ext_power_api *)dev->api;

    if (api->get == NULL) {
        return -ENOTSUP;
    }

    return api->get(dev);
}

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#include <syscalls/ext_power.h>
