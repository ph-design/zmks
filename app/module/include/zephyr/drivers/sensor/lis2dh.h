/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_LIS2DH_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_LIS2DH_H_

#include <zephyr/drivers/sensor.h>

enum lis2dh_self_test {
    LIS2DH_SELF_TEST_DISABLE = 0,
    LIS2DH_SELF_TEST_POSITIVE = 1,
    LIS2DH_SELF_TEST_NEGATIVE = 2,
};

enum sensor_attribute_lis2dh {
    SENSOR_ATTR_LIS2DH_SELF_TEST = SENSOR_ATTR_PRIV_START,
    /* click threshold in raw counts (CLICK_THS) */
    SENSOR_ATTR_LIS2DH_CLICK_THS,
    /* click max duration, dead time and double-click window, in milliseconds */
    SENSOR_ATTR_LIS2DH_CLICK_TIME_LIMIT_MS,
    SENSOR_ATTR_LIS2DH_CLICK_LATENCY_MS,
    SENSOR_ATTR_LIS2DH_CLICK_WINDOW_MS,
    SENSOR_ATTR_LIS2DH_ACT_THS,
    SENSOR_ATTR_LIS2DH_ACT_DUR_MS,
    SENSOR_ATTR_LIS2DH_CLICK_AXES,
};

enum sensor_channel_lis2dh {
    SENSOR_CHAN_LIS2DH_ORIENTATION = SENSOR_CHAN_PRIV_START,
    /* raw CLICK_SRC byte latched at the last click interrupt */
    SENSOR_CHAN_LIS2DH_CLICK_SRC,
};

/* CLICK_SRC bit layout (datasheet Table 73) */
#define LIS2DH_CLICK_SRC_IA BIT(7)
#define LIS2DH_CLICK_SRC_DCLICK BIT(6)
#define LIS2DH_CLICK_SRC_SCLICK BIT(5)
#define LIS2DH_CLICK_SRC_SIGN BIT(4)
#define LIS2DH_CLICK_SRC_Z BIT(2)
#define LIS2DH_CLICK_SRC_Y BIT(1)
#define LIS2DH_CLICK_SRC_X BIT(0)

enum lis2dh_orientation {
    LIS2DH_ORIENT_UNKNOWN = 0,
    LIS2DH_ORIENT_FLAT_UP = 1,
    LIS2DH_ORIENT_FLAT_DOWN = 2,
    LIS2DH_ORIENT_TILTED = 3,
};

#endif /* ZEPHYR_INCLUDE_DRIVERS_SENSOR_LIS2DH_H_ */
