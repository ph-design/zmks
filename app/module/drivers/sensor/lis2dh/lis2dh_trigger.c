/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zmk_lis2dh

#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor/lis2dh.h>

#define START_TRIG_INT1 0
#define START_TRIG_INT2 1
#define TRIGGED_INT1 4
#define TRIGGED_INT2 5

LOG_MODULE_DECLARE(lis2dh, CONFIG_SENSOR_LOG_LEVEL);
#include "lis2dh.h"

static const gpio_flags_t gpio_int_cfg[5] = {
    GPIO_INT_EDGE_BOTH,  GPIO_INT_EDGE_RISING, GPIO_INT_EDGE_FALLING,
    GPIO_INT_LEVEL_HIGH, GPIO_INT_LEVEL_LOW,
};

static inline void setup_int1(const struct device *dev, bool enable) {
    const struct lis2dh_config *cfg = dev->config;

    gpio_pin_interrupt_configure_dt(&cfg->gpio_drdy,
                                    enable ? gpio_int_cfg[cfg->int1_mode] : GPIO_INT_DISABLE);
}

static int lis2dh_trigger_drdy_set(const struct device *dev, enum sensor_channel chan,
                                   sensor_trigger_handler_t handler,
                                   const struct sensor_trigger *trig) {
    const struct lis2dh_config *cfg = dev->config;
    struct lis2dh_data *lis2dh = dev->data;
    int status;

    if (cfg->gpio_drdy.port == NULL) {
        LOG_ERR("trigger_set DRDY int not supported");
        return -ENOTSUP;
    }

    setup_int1(dev, false);

    // cancel pending trigger
    atomic_clear_bit(&lis2dh->trig_flags, TRIGGED_INT1);

    status = lis2dh->hw_tf->update_reg(dev, LIS2DH_REG_CTRL3, LIS2DH_EN_DRDY1_INT1, 0);

    lis2dh->handler_drdy = handler;
    lis2dh->trig_drdy = trig;
    if ((handler == NULL) || (status < 0)) {
        return status;
    }

    lis2dh->chan_drdy = chan;

    // serialize int1 start in thread to sync output sampling with first interrupt
    atomic_set_bit(&lis2dh->trig_flags, START_TRIG_INT1);
#if defined(CONFIG_ZMK_LIS2DH_TRIGGER_OWN_THREAD)
    k_sem_give(&lis2dh->gpio_sem);
#elif defined(CONFIG_ZMK_LIS2DH_TRIGGER_GLOBAL_THREAD)
    k_work_submit(&lis2dh->work);
#endif

    return 0;
}

static int lis2dh_start_trigger_int1(const struct device *dev) {
    int status;
    uint8_t raw[LIS2DH_BUF_SZ];
    uint8_t ctrl1 = 0U;
    struct lis2dh_data *lis2dh = dev->data;

    // power down temporarily to align interrupt & data sampling
    status = lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CTRL1, &ctrl1);
    if (unlikely(status < 0)) {
        return status;
    }
    status = lis2dh->hw_tf->write_reg(dev, LIS2DH_REG_CTRL1, ctrl1 & ~LIS2DH_ODR_MASK);

    if (unlikely(status < 0)) {
        return status;
    }

    LOG_DBG("ctrl1=0x%x @tick=%u", ctrl1, k_cycle_get_32());

    // flush output data
    status = lis2dh->hw_tf->read_data(dev, LIS2DH_REG_STATUS, raw, sizeof(raw));
    if (unlikely(status < 0)) {
        return status;
    }

    setup_int1(dev, true);

    // re-enable output sampling
    status = lis2dh->hw_tf->write_reg(dev, LIS2DH_REG_CTRL1, ctrl1);
    if (unlikely(status < 0)) {
        return status;
    }

    return lis2dh->hw_tf->update_reg(dev, LIS2DH_REG_CTRL3, LIS2DH_EN_DRDY1_INT1,
                                     LIS2DH_EN_DRDY1_INT1);
}

#define LIS2DH_ANYM_CFG                                                                            \
    (LIS2DH_INT_CFG_ZHIE_ZUPE | LIS2DH_INT_CFG_ZLIE_ZDOWNE | LIS2DH_INT_CFG_YHIE_YUPE |            \
     LIS2DH_INT_CFG_YLIE_YDOWNE | LIS2DH_INT_CFG_XHIE_XUPE | LIS2DH_INT_CFG_XLIE_XDOWNE)

static inline void setup_int2(const struct device *dev, bool enable) {
    const struct lis2dh_config *cfg = dev->config;

    gpio_pin_interrupt_configure_dt(&cfg->gpio_int,
                                    enable ? gpio_int_cfg[cfg->int2_mode] : GPIO_INT_DISABLE);
}

static int lis2dh_ctrl2_sync(const struct device *dev);

static int lis2dh_trigger_anym_tap_set(const struct device *dev, sensor_trigger_handler_t handler,
                                       const struct sensor_trigger *trig) {
    const struct lis2dh_config *cfg = dev->config;
    struct lis2dh_data *lis2dh = dev->data;
    int status;
    uint8_t reg_val;

    if (cfg->gpio_int.port == NULL) {
        LOG_ERR("trigger_set AnyMotion int not supported");
        return -ENOTSUP;
    }

    setup_int2(dev, false);

    // cancel pending trigger
    atomic_clear_bit(&lis2dh->trig_flags, TRIGGED_INT2);

    if (cfg->hw.anym_on_int1) {
        status = lis2dh->hw_tf->update_reg(dev, LIS2DH_REG_CTRL3, LIS2DH_EN_DRDY1_INT1, 0);
        if (status < 0) {
            return status;
        }
    }

    // disable any-motion events
    status = lis2dh->hw_tf->write_reg(
        dev, cfg->hw.anym_on_int1 ? LIS2DH_REG_INT1_CFG : LIS2DH_REG_INT2_CFG, 0);
    if (status < 0) {
        return status;
    }

    // disable click events
    status = lis2dh->hw_tf->write_reg(dev, LIS2DH_REG_CFG_CLICK, 0);
    if (status < 0) {
        return status;
    }

    // clear pending interrupts
    status = lis2dh->hw_tf->read_reg(
        dev, cfg->hw.anym_on_int1 ? LIS2DH_REG_INT1_SRC : LIS2DH_REG_INT2_SRC, &reg_val);
    if (status < 0) {
        return status;
    }

    status = lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CLICK_SRC, &reg_val);

    if (trig->type == SENSOR_TRIG_DELTA) {
        lis2dh->handler_anymotion = handler;
        lis2dh->trig_anymotion = trig;
    } else if (trig->type == SENSOR_TRIG_TAP) {
        lis2dh->handler_tap = handler;
        lis2dh->trig_tap = trig;
    } else if (trig->type == SENSOR_TRIG_DOUBLE_TAP) {
        lis2dh->handler_dtap = handler;
        lis2dh->trig_dtap = trig;
    }

    status = lis2dh_ctrl2_sync(dev);
    if (status < 0) {
        LOG_ERR("Failed to re-sync high-pass filter routing");
        return status;
    }

    if ((handler == NULL) || (status < 0)) {
        return status;
    }

    atomic_set_bit(&lis2dh->trig_flags, START_TRIG_INT2);
#if defined(CONFIG_ZMK_LIS2DH_TRIGGER_OWN_THREAD)
    k_sem_give(&lis2dh->gpio_sem);
#elif defined(CONFIG_ZMK_LIS2DH_TRIGGER_GLOBAL_THREAD)
    k_work_submit(&lis2dh->work);
#endif
    return 0;
}

static int lis2dh_trigger_anym_set(const struct device *dev, sensor_trigger_handler_t handler,
                                   const struct sensor_trigger *trig) {
    return lis2dh_trigger_anym_tap_set(dev, handler, trig);
}

static int lis2dh_trigger_tap_set(const struct device *dev, sensor_trigger_handler_t handler,
                                  const struct sensor_trigger *trig) {
    return lis2dh_trigger_anym_tap_set(dev, handler, trig);
}

static int lis2dh_trigger_dtap_set(const struct device *dev, sensor_trigger_handler_t handler,
                                   const struct sensor_trigger *trig) {
    return lis2dh_trigger_anym_tap_set(dev, handler, trig);
}

static int lis2dh_ctrl2_sync(const struct device *dev) {
    const struct lis2dh_config *cfg = dev->config;
    struct lis2dh_data *lis2dh = dev->data;
    bool anym_hpf = (cfg->hw.anym_mode & 0x1) == 0;
    bool has_anym = lis2dh->handler_anymotion != NULL;
    bool has_click = lis2dh->handler_tap != NULL || lis2dh->handler_dtap != NULL;
    uint8_t mask = LIS2DH_HPM0_EN_BIT | LIS2DH_HPM1_EN_BIT | LIS2DH_HPIS1_EN_BIT |
                   LIS2DH_HPIS2_EN_BIT | LIS2DH_HPCLICK_EN_BIT;
    uint8_t val = 0;

    if (has_anym && anym_hpf) {
        val |= LIS2DH_HPIS1_EN_BIT | LIS2DH_HPIS2_EN_BIT;
    }
    if (has_click) {
        val |= LIS2DH_HPCLICK_EN_BIT;
    }
    if (val != 0) {
        val |= LIS2DH_HPM0_EN_BIT | LIS2DH_HPM1_EN_BIT;
    }

    return lis2dh->hw_tf->update_reg(dev, LIS2DH_REG_CTRL2, mask, val);
}

static int lis2dh_start_trigger_int2(const struct device *dev) {
    struct lis2dh_data *lis2dh = dev->data;
    const struct lis2dh_config *cfg = dev->config;
    int status = 0;
    uint8_t reg = 0, mask = 0, val = 0;

    setup_int2(dev, true);

    bool has_anyt = (lis2dh->handler_tap != NULL);
    bool has_dtap = (lis2dh->handler_dtap != NULL);
    bool has_anym = (lis2dh->handler_anymotion != NULL);

    reg = cfg->hw.anym_on_int1 ? LIS2DH_REG_INT1_CFG : LIS2DH_REG_INT2_CFG;
    val = (cfg->hw.anym_mode << LIS2DH_INT_CFG_MODE_SHIFT) | LIS2DH_ANYM_CFG;
    status = lis2dh->hw_tf->write_reg(dev, reg, val);
    if (status < 0) {
        LOG_ERR("Failed to configure any motion interrupt");
        return status;
    }

    // latch interrupt so INTx_SRC holds zone bits until read
    if (cfg->hw.anym_latch) {
        mask = cfg->hw.anym_on_int1 ? LIS2DH_EN_LIR_INT1 : LIS2DH_EN_LIR_INT2;
        status = lis2dh->hw_tf->update_reg(dev, LIS2DH_REG_CTRL5, mask, mask);
        if (status < 0) {
            LOG_ERR("Failed to latch any motion interrupt");
            return status;
        }
    }

    status = lis2dh_ctrl2_sync(dev);
    if (status < 0) {
        LOG_ERR("Failed to route high-pass filter");
        return status;
    }
    status = lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_REFERENCE, &reg);
    if (status < 0) {
        LOG_ERR("Failed to reset high-pass filter reference");
        return status;
    }

    reg = cfg->hw.anym_on_int1 ? LIS2DH_REG_CTRL3 : LIS2DH_REG_CTRL6;
    mask = cfg->hw.anym_on_int1 ? LIS2DH_EN_IA_INT1 : LIS2DH_EN_IA_INT2;
    val = has_anym ? mask : 0;
    status = lis2dh->hw_tf->update_reg(dev, reg, mask, val);
    if (status < 0) {
        LOG_ERR("Failed to enable any motion detection on int line");
        return status;
    }

    reg = LIS2DH_REG_CFG_CLICK;
    mask = LIS2DH_EN_CLICK_XS | LIS2DH_EN_CLICK_YS | LIS2DH_EN_CLICK_ZS | LIS2DH_EN_CLICK_XD |
           LIS2DH_EN_CLICK_YD | LIS2DH_EN_CLICK_ZD;
    val = (has_anyt ? LIS2DH_EN_CLICK_YS : 0) | (has_dtap ? LIS2DH_EN_CLICK_YD : 0);
    status = lis2dh->hw_tf->update_reg(dev, reg, mask, val);
    if (status < 0) {
        LOG_ERR("Failed to configure tap interrupt");
        return status;
    }

    reg = cfg->hw.anym_on_int1 ? LIS2DH_REG_CTRL3 : LIS2DH_REG_CTRL6;
    mask = cfg->hw.anym_on_int1 ? LIS2DH_EN_CLICK_INT1 : LIS2DH_EN_CLICK_INT2;
    val = (has_anyt || has_dtap) ? mask : 0;
    status = lis2dh->hw_tf->update_reg(dev, reg, mask, val);
    if (status < 0) {
        LOG_ERR("Failed to enable click detection on int line");
        return status;
    }
    // diagnostics readback; ctrl0 only exists on LIS2DH12 (0x00 on LIS3DH)
    uint8_t rb0 = 0, rb2 = 0, rb5 = 0, rbcfg = 0, rbths = 0, rb6 = 0;
    uint8_t rbcc = 0, rbcths = 0, rbtl = 0, rblat = 0, rbwin = 0;
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CTRL0, &rb0);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CTRL2, &rb2);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CTRL5, &rb5);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_INT2_CFG, &rbcfg);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_INT2_THS, &rbths);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CTRL6, &rb6);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CFG_CLICK, &rbcc);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CFG_CLICK_THS, &rbcths);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_TIME_LIMIT, &rbtl);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_TIME_LATENCY, &rblat);
    lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_TIME_WINDOW, &rbwin);
    LOG_INF("int2 arm: ctrl0=%02x ctrl2=%02x ctrl5=%02x int2_cfg=%02x ths=%02x ctrl6=%02x", rb0,
            rb2, rb5, rbcfg, rbths, rb6);
    LOG_INF("click arm: cfg=%02x ths=%02x limit=%02x latency=%02x window=%02x", rbcc, rbcths, rbtl,
            rblat, rbwin);
    return 0;
}

int lis2dh_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
                       sensor_trigger_handler_t handler) {
    if (trig->type == SENSOR_TRIG_DATA_READY && trig->chan == SENSOR_CHAN_ACCEL_XYZ) {
        return lis2dh_trigger_drdy_set(dev, trig->chan, handler, trig);
    } else if (trig->type == SENSOR_TRIG_DELTA) {
        return lis2dh_trigger_anym_set(dev, handler, trig);
    } else if (trig->type == SENSOR_TRIG_TAP) {
        return lis2dh_trigger_tap_set(dev, handler, trig);
    } else if (trig->type == SENSOR_TRIG_DOUBLE_TAP) {
        return lis2dh_trigger_dtap_set(dev, handler, trig);
    }

    return -ENOTSUP;
}

// ODR Hz from CTRL1; low-power index 9 = 5376 Hz
static int lis2dh_current_odr_hz(const struct device *dev) {
    static const uint16_t odr_hz[] = {0, 1, 10, 25, 50, 100, 200, 400, 1620, 1344, 5376};
    struct lis2dh_data *lis2dh = dev->data;
    uint8_t ctrl1;
    int status = lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CTRL1, &ctrl1);

    if (status < 0) {
        return status;
    }

    uint8_t idx = (ctrl1 & LIS2DH_ODR_MASK) >> LIS2DH_ODR_SHIFT;

    if (idx == LIS2DH_ODR_9 && (ctrl1 & LIS2DH_LP_EN_BIT_MASK)) {
        idx++;
    }

    return odr_hz[idx];
}

// click timing registers: 1 LSb = 1/ODR; TIME_LIMIT is 7-bit, rest 8-bit
static int lis2dh_click_time_set(const struct device *dev, uint8_t reg, int32_t ms,
                                 uint8_t max_count) {
    int odr = lis2dh_current_odr_hz(dev);
    uint32_t count;

    if (odr <= 0) {
        return odr < 0 ? odr : -EINVAL;
    }
    if (ms < 0) {
        return -EINVAL;
    }

    count = (uint32_t)ms * (uint32_t)odr / 1000;
    if (count > max_count) {
        return -EINVAL;
    }

    struct lis2dh_data *lis2dh = dev->data;

    return lis2dh->hw_tf->write_reg(dev, reg, (uint8_t)count);
}

int lis2dh_acc_slope_config(const struct device *dev, enum sensor_attribute attr,
                            const struct sensor_value *val) {
    struct lis2dh_data *lis2dh = dev->data;
    const struct lis2dh_config *cfg = dev->config;
    int status;

    if (attr == SENSOR_ATTR_SLOPE_TH) {
        uint8_t range_g, reg_val;
        uint32_t slope_th_ums2;

        status = lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CTRL4, &reg_val);
        if (status < 0) {
            return status;
        }

        // FS value: 0 = 2g .. 3 = 16g
        range_g = 2 * (1 << ((LIS2DH_FS_MASK & reg_val) >> LIS2DH_FS_SHIFT));

        slope_th_ums2 = val->val1 * 1000000 + val->val2;

        // threshold must stay within range
        if ((slope_th_ums2 - 1) > (range_g * SENSOR_G)) {
            return -EINVAL;
        }

        // 7-bit full-range value
        reg_val = 128 / range_g * (slope_th_ums2 - 1) / SENSOR_G;

        LOG_INF("int2_ths=0x%x range_g=%d ums2=%u", reg_val, range_g, slope_th_ums2 - 1);

        status = lis2dh->hw_tf->write_reg(
            dev, cfg->hw.anym_on_int1 ? LIS2DH_REG_INT1_THS : LIS2DH_REG_INT2_THS, reg_val);
    } else if (attr == SENSOR_ATTR_SLOPE_DUR) {
        // slope duration in samples: N/ODR
        if (val->val1 < 0 || val->val1 > 127) {
            return -ENOTSUP;
        }

        LOG_INF("int2_dur=0x%x", val->val1);

        status = lis2dh->hw_tf->write_reg(
            dev, cfg->hw.anym_on_int1 ? LIS2DH_REG_INT1_DUR : LIS2DH_REG_INT2_DUR, val->val1);
    } else if ((int)attr == SENSOR_ATTR_LIS2DH_CLICK_THS) {
        if (val->val1 < 1 || val->val1 > 127) {
            return -EINVAL;
        }
        status =
            lis2dh->hw_tf->write_reg(dev, LIS2DH_REG_CFG_CLICK_THS, LIS2DH_CLICK_LIR | val->val1);
    } else if ((int)attr == SENSOR_ATTR_LIS2DH_CLICK_TIME_LIMIT_MS) {
        status = lis2dh_click_time_set(dev, LIS2DH_REG_TIME_LIMIT, val->val1, 127);
    } else if ((int)attr == SENSOR_ATTR_LIS2DH_CLICK_LATENCY_MS) {
        status = lis2dh_click_time_set(dev, LIS2DH_REG_TIME_LATENCY, val->val1, 255);
    } else if ((int)attr == SENSOR_ATTR_LIS2DH_CLICK_WINDOW_MS) {
        status = lis2dh_click_time_set(dev, LIS2DH_REG_TIME_WINDOW, val->val1, 255);
    } else if ((int)attr == SENSOR_ATTR_LIS2DH_ACT_THS) {
        if (val->val1 < 0 || val->val1 > 127) {
            return -EINVAL;
        }

        LOG_INF("act_ths=0x%x", val->val1);

        // 0 disables sleep-to-wake, nonzero arms it
        status = lis2dh->hw_tf->write_reg(dev, LIS2DH_REG_ACT_THS, val->val1);
    } else if ((int)attr == SENSOR_ATTR_LIS2DH_ACT_DUR_MS) {
        // datasheet 3.2.4: duration = (8*N+1)/ODR, so N = (ms*ODR/1000 - 1)/8
        int odr = lis2dh_current_odr_hz(dev);

        if (odr <= 0) {
            return odr < 0 ? odr : -EINVAL;
        }
        if (val->val1 < 0) {
            return -EINVAL;
        }

        uint32_t count = ((uint32_t)val->val1 * (uint32_t)odr / 1000 - 1) / 8;

        if (count > 255) {
            return -EINVAL;
        }

        LOG_INF("act_dur=0x%x", count);

        status = lis2dh->hw_tf->write_reg(dev, LIS2DH_REG_ACT_DUR, count);
    } else {
        status = -ENOTSUP;
    }

    return status;
}

static void lis2dh_gpio_int1_callback(const struct device *dev, struct gpio_callback *cb,
                                      uint32_t pins) {
    struct lis2dh_data *lis2dh = CONTAINER_OF(cb, struct lis2dh_data, gpio_int1_cb);

    ARG_UNUSED(pins);

    atomic_set_bit(&lis2dh->trig_flags, TRIGGED_INT1);

    // level triggered: disable until processed
    setup_int1(lis2dh->dev, false);

#if defined(CONFIG_ZMK_LIS2DH_TRIGGER_OWN_THREAD)
    k_sem_give(&lis2dh->gpio_sem);
#elif defined(CONFIG_ZMK_LIS2DH_TRIGGER_GLOBAL_THREAD)
    k_work_submit(&lis2dh->work);
#endif
}

static void lis2dh_gpio_int2_callback(const struct device *dev, struct gpio_callback *cb,
                                      uint32_t pins) {
    struct lis2dh_data *lis2dh = CONTAINER_OF(cb, struct lis2dh_data, gpio_int2_cb);

    ARG_UNUSED(pins);

    atomic_set_bit(&lis2dh->trig_flags, TRIGGED_INT2);

    // level triggered: disable until processed
    setup_int2(lis2dh->dev, false);

#if defined(CONFIG_ZMK_LIS2DH_TRIGGER_OWN_THREAD)
    k_sem_give(&lis2dh->gpio_sem);
#elif defined(CONFIG_ZMK_LIS2DH_TRIGGER_GLOBAL_THREAD)
    k_work_submit(&lis2dh->work);
#endif
}

static void lis2dh_thread_cb(const struct device *dev) {
    struct lis2dh_data *lis2dh = dev->data;
    const struct lis2dh_config *cfg = dev->config;
    int status;

    if (cfg->gpio_drdy.port &&
        unlikely(atomic_test_and_clear_bit(&lis2dh->trig_flags, START_TRIG_INT1))) {
        status = lis2dh_start_trigger_int1(dev);

        if (unlikely(status < 0)) {
            LOG_ERR("lis2dh_start_trigger_int1: %d", status);
        }
        return;
    }

    if (cfg->gpio_int.port &&
        unlikely(atomic_test_and_clear_bit(&lis2dh->trig_flags, START_TRIG_INT2))) {
        status = lis2dh_start_trigger_int2(dev);

        if (unlikely(status < 0)) {
            LOG_ERR("lis2dh_start_trigger_int2: %d", status);
        }
        return;
    }

    if (cfg->gpio_drdy.port && atomic_test_and_clear_bit(&lis2dh->trig_flags, TRIGGED_INT1)) {
        if (likely(lis2dh->handler_drdy != NULL)) {
            lis2dh->handler_drdy(dev, lis2dh->trig_drdy);
        }

        // re-enable if handler didn't disable itself
        if (likely(lis2dh->handler_drdy != NULL)) {
            setup_int1(dev, true);
        }

        return;
    }

    if (cfg->gpio_int.port && atomic_test_and_clear_bit(&lis2dh->trig_flags, TRIGGED_INT2)) {
        uint8_t reg_val = 0;

        // reading SRC clears the interrupt and de-asserts the line
        status = lis2dh->hw_tf->read_reg(
            dev, cfg->hw.anym_on_int1 ? LIS2DH_REG_INT1_SRC : LIS2DH_REG_INT2_SRC, &reg_val);
        if (status < 0) {
            LOG_ERR("clearing interrupt 2 failed: %d", status);
            return;
        }

        // throttled log of latched SRC (IA=bit6, axes=[5:0])
        static uint8_t src_log_div;
        if (++src_log_div >= 16) {
            src_log_div = 0;
            LOG_INF("int2 src=%02x", reg_val);
        }

        if (likely(lis2dh->handler_anymotion != NULL) && (reg_val >> LIS2DH_INT_CFG_MODE_SHIFT)) {
            lis2dh->handler_anymotion(dev, lis2dh->trig_anymotion);
        }

        status = lis2dh->hw_tf->read_reg(dev, LIS2DH_REG_CLICK_SRC, &reg_val);
        if (status < 0) {
            LOG_ERR("clearing interrupt 2 failed: %d", status);
            return;
        }

        if ((reg_val & (LIS2DH_CLICK_SRC_SCLICK | LIS2DH_CLICK_SRC_DCLICK)) != 0) {
            lis2dh->click_src = reg_val;
        }

        if (likely(lis2dh->handler_tap != NULL) && (reg_val & LIS2DH_CLICK_SRC_SCLICK)) {
            lis2dh->handler_tap(dev, lis2dh->trig_tap);
        }

        if (likely(lis2dh->handler_dtap != NULL) && (reg_val & LIS2DH_CLICK_SRC_DCLICK)) {
            lis2dh->handler_dtap(dev, lis2dh->trig_dtap);
        }

        // re-enable if handler didn't disable itself
        if (lis2dh->handler_anymotion || lis2dh->handler_tap || lis2dh->handler_dtap) {
            setup_int2(dev, true);
        }

        return;
    }
}

#ifdef CONFIG_ZMK_LIS2DH_TRIGGER_OWN_THREAD
static void lis2dh_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    struct lis2dh_data *lis2dh = p1;

    while (1) {
        k_sem_take(&lis2dh->gpio_sem, K_FOREVER);
        lis2dh_thread_cb(lis2dh->dev);
    }
}
#endif

#ifdef CONFIG_ZMK_LIS2DH_TRIGGER_GLOBAL_THREAD
static void lis2dh_work_cb(struct k_work *work) {
    struct lis2dh_data *lis2dh = CONTAINER_OF(work, struct lis2dh_data, work);

    lis2dh_thread_cb(lis2dh->dev);
}
#endif

int lis2dh_init_interrupt(const struct device *dev) {
    struct lis2dh_data *lis2dh = dev->data;
    const struct lis2dh_config *cfg = dev->config;
    int status;

    lis2dh->dev = dev;

#if defined(CONFIG_ZMK_LIS2DH_TRIGGER_OWN_THREAD)
    k_sem_init(&lis2dh->gpio_sem, 0, K_SEM_MAX_LIMIT);

    k_thread_create(&lis2dh->thread, lis2dh->thread_stack, CONFIG_ZMK_LIS2DH_THREAD_STACK_SIZE,
                    lis2dh_thread, lis2dh, NULL, NULL, K_PRIO_COOP(CONFIG_ZMK_LIS2DH_THREAD_PRIORITY),
                    0, K_NO_WAIT);
#elif defined(CONFIG_ZMK_LIS2DH_TRIGGER_GLOBAL_THREAD)
    k_work_init(&lis2dh->work, lis2dh_work_cb);
#endif

    // setup INT1 (DRDY) if defined in DT
    if (!gpio_is_ready_dt(&cfg->gpio_drdy)) {
        // API may return false even when ptr is NULL
        if (cfg->gpio_drdy.port != NULL) {
            LOG_ERR("GPIO device not ready: %s", cfg->gpio_drdy.port->name);
            return -ENODEV;
        }

        LOG_DBG("gpio_drdy not defined in DT");
        status = 0;
        goto check_gpio_int;
    }

    status = gpio_pin_configure_dt(&cfg->gpio_drdy, GPIO_INPUT);
    if (status < 0) {
        LOG_ERR("Could not configure %s.%02u", cfg->gpio_drdy.port->name, cfg->gpio_drdy.pin);
        return status;
    }

    gpio_init_callback(&lis2dh->gpio_int1_cb, lis2dh_gpio_int1_callback, BIT(cfg->gpio_drdy.pin));

    status = gpio_add_callback(cfg->gpio_drdy.port, &lis2dh->gpio_int1_cb);
    if (status < 0) {
        LOG_ERR("Could not add gpio int1 callback");
        return status;
    }

    LOG_INF("%s: int1 on %s.%02u", dev->name, cfg->gpio_drdy.port->name, cfg->gpio_drdy.pin);

check_gpio_int:
    // setup INT2 (any-motion) if defined in DT
    if (!gpio_is_ready_dt(&cfg->gpio_int)) {
        // API may return false even when ptr is NULL
        if (cfg->gpio_int.port != NULL) {
            LOG_ERR("GPIO device not ready: %s", cfg->gpio_int.port->name);
            return -ENODEV;
        }

        LOG_DBG("gpio_int not defined in DT");
        status = 0;
        goto end;
    }

    status = gpio_pin_configure_dt(&cfg->gpio_int, GPIO_INPUT);
    if (status < 0) {
        LOG_ERR("Could not configure %s.%02u", cfg->gpio_int.port->name, cfg->gpio_int.pin);
        return status;
    }

    gpio_init_callback(&lis2dh->gpio_int2_cb, lis2dh_gpio_int2_callback, BIT(cfg->gpio_int.pin));

    // interrupt enabled later by trigger set
    status = gpio_add_callback(cfg->gpio_int.port, &lis2dh->gpio_int2_cb);
    if (status < 0) {
        LOG_ERR("Could not add gpio int2 callback (%d)", status);
        return status;
    }

    LOG_INF("%s: int2 on %s.%02u", dev->name, cfg->gpio_int.port->name, cfg->gpio_int.pin);

    // disable interrupt in case of warm (re)boot
    status = lis2dh->hw_tf->write_reg(
        dev, cfg->hw.anym_on_int1 ? LIS2DH_REG_INT1_CFG : LIS2DH_REG_INT2_CFG, 0);
    if (status < 0) {
        LOG_ERR("Interrupt disable reg write failed (%d)", status);
        return status;
    }
    status = lis2dh->hw_tf->write_reg(dev, LIS2DH_REG_CFG_CLICK, 0);
    if (status < 0) {
        LOG_ERR("Interrupt disable reg write failed (%d)", status);
        return status;
    }

end:
    return status;
}
