/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_lis2de12

#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor/lis2de12.h>

#define START_TRIG_INT1			0
#define START_TRIG_INT2			1
#define TRIGGED_INT1			4
#define TRIGGED_INT2			5

LOG_MODULE_DECLARE(lis2de12, CONFIG_SENSOR_LOG_LEVEL);
#include "lis2de12.h"

static const gpio_flags_t gpio_int_cfg[5] = {
			GPIO_INT_EDGE_BOTH,
			GPIO_INT_EDGE_RISING,
			GPIO_INT_EDGE_FALLING,
			GPIO_INT_LEVEL_HIGH,
			GPIO_INT_LEVEL_LOW,
			};

static inline void setup_int1(const struct device *dev,
			      bool enable)
{
	const struct lis2de12_config *cfg = dev->config;

	gpio_pin_interrupt_configure_dt(&cfg->gpio_drdy,
					enable
					? gpio_int_cfg[cfg->int1_mode]
					: GPIO_INT_DISABLE);
}

static int lis2de12_trigger_drdy_set(const struct device *dev,
				   enum sensor_channel chan,
				   sensor_trigger_handler_t handler,
				   const struct sensor_trigger *trig)
{
	const struct lis2de12_config *cfg = dev->config;
	struct lis2de12_data *lis2de12 = dev->data;
	int status;

	if (cfg->gpio_drdy.port == NULL) {
		LOG_ERR("trigger_set DRDY int not supported");
		return -ENOTSUP;
	}

	setup_int1(dev, false);

	/* cancel potentially pending trigger */
	atomic_clear_bit(&lis2de12->trig_flags, TRIGGED_INT1);

	status = lis2de12->hw_tf->update_reg(dev, LIS2DE12_REG_CTRL3,
					   LIS2DE12_EN_DRDY1_INT1, 0);

	lis2de12->handler_drdy = handler;
	lis2de12->trig_drdy = trig;
	if ((handler == NULL) || (status < 0)) {
		return status;
	}

	lis2de12->chan_drdy = chan;

	/* serialize start of int1 in thread to synchronize output sampling
	 * and first interrupt. this avoids concurrent bus context access.
	 */
	atomic_set_bit(&lis2de12->trig_flags, START_TRIG_INT1);
#if defined(CONFIG_LIS2DE12_TRIGGER_OWN_THREAD)
	k_sem_give(&lis2de12->gpio_sem);
#elif defined(CONFIG_LIS2DE12_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&lis2de12->work);
#endif

	return 0;
}

static int lis2de12_start_trigger_int1(const struct device *dev)
{
	int status;
	uint8_t raw[LIS2DE12_BUF_SZ];
	uint8_t ctrl1 = 0U;
	struct lis2de12_data *lis2de12 = dev->data;

	/* power down temporarily to align interrupt & data output sampling */
	status = lis2de12->hw_tf->read_reg(dev, LIS2DE12_REG_CTRL1, &ctrl1);
	if (unlikely(status < 0)) {
		return status;
	}
	status = lis2de12->hw_tf->write_reg(dev, LIS2DE12_REG_CTRL1,
					  ctrl1 & ~LIS2DE12_ODR_MASK);

	if (unlikely(status < 0)) {
		return status;
	}

	LOG_DBG("ctrl1=0x%x @tick=%u", ctrl1, k_cycle_get_32());

	/* empty output data */
	status = lis2de12->hw_tf->read_data(dev, LIS2DE12_REG_STATUS,
					  raw, sizeof(raw));
	if (unlikely(status < 0)) {
		return status;
	}

	setup_int1(dev, true);

	/* re-enable output sampling */
	status = lis2de12->hw_tf->write_reg(dev, LIS2DE12_REG_CTRL1, ctrl1);
	if (unlikely(status < 0)) {
		return status;
	}

	return lis2de12->hw_tf->update_reg(dev, LIS2DE12_REG_CTRL3,
					 LIS2DE12_EN_DRDY1_INT1,
					 LIS2DE12_EN_DRDY1_INT1);
}

#define LIS2DE12_ANYM_CFG (LIS2DE12_INT_CFG_ZHIE_ZUPE | LIS2DE12_INT_CFG_ZLIE_ZDOWNE |\
			LIS2DE12_INT_CFG_YHIE_YUPE | LIS2DE12_INT_CFG_YLIE_YDOWNE |\
			LIS2DE12_INT_CFG_XHIE_XUPE | LIS2DE12_INT_CFG_XLIE_XDOWNE)

static inline void setup_int2(const struct device *dev,
			      bool enable)
{
	const struct lis2de12_config *cfg = dev->config;

	gpio_pin_interrupt_configure_dt(&cfg->gpio_int,
					enable
					? gpio_int_cfg[cfg->int2_mode]
					: GPIO_INT_DISABLE);
}

/* common handler for any motion and tap triggers */
static int lis2de12_trigger_anym_tap_set(const struct device *dev,
				       sensor_trigger_handler_t handler,
				       const struct sensor_trigger *trig)
{
	const struct lis2de12_config *cfg = dev->config;
	struct lis2de12_data *lis2de12 = dev->data;
	int status;
	uint8_t reg_val;

	if (cfg->gpio_int.port == NULL) {
		LOG_ERR("trigger_set AnyMotion int not supported");
		return -ENOTSUP;
	}

	setup_int2(dev, false);

	/* cancel potentially pending trigger */
	atomic_clear_bit(&lis2de12->trig_flags, TRIGGED_INT2);

	if (cfg->hw.anym_on_int1) {
		status = lis2de12->hw_tf->update_reg(dev, LIS2DE12_REG_CTRL3,
						   LIS2DE12_EN_DRDY1_INT1, 0);
	}

	/* disable any movement interrupt events */
	status = lis2de12->hw_tf->write_reg(dev,
					  cfg->hw.anym_on_int1 ?
						LIS2DE12_REG_INT1_CFG :
						LIS2DE12_REG_INT2_CFG,
					  0);
	/* disable any click interrupt events */
	status = lis2de12->hw_tf->write_reg(dev,
					  LIS2DE12_REG_CFG_CLICK,
					  0);

	/* make sure any pending interrupt is cleared */
	status = lis2de12->hw_tf->read_reg(dev,
					 cfg->hw.anym_on_int1 ?
						LIS2DE12_REG_INT1_SRC :
						LIS2DE12_REG_INT2_SRC,
					 &reg_val);
	status = lis2de12->hw_tf->read_reg(dev,
					 LIS2DE12_REG_CLICK_SRC,
					 &reg_val);

	if (trig->type == SENSOR_TRIG_DELTA) {
		lis2de12->handler_anymotion = handler;
		lis2de12->trig_anymotion = trig;
	} else if (trig->type == SENSOR_TRIG_TAP) {
		lis2de12->handler_tap = handler;
		lis2de12->trig_tap = trig;
	} else if (trig->type == SENSOR_TRIG_DOUBLE_TAP) {
		lis2de12->handler_dtap = handler;
		lis2de12->trig_dtap = trig;
	}

	if ((handler == NULL) || (status < 0)) {
		return status;
	}

	/* serialize start of int2 in thread to synchronize output sampling
	 * and first interrupt. this avoids concurrent bus context access.
	 */
	atomic_set_bit(&lis2de12->trig_flags, START_TRIG_INT2);
#if defined(CONFIG_LIS2DE12_TRIGGER_OWN_THREAD)
	k_sem_give(&lis2de12->gpio_sem);
#elif defined(CONFIG_LIS2DE12_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&lis2de12->work);
#endif
	return 0;
}

static int lis2de12_trigger_anym_set(const struct device *dev,
				   sensor_trigger_handler_t handler,
				   const struct sensor_trigger *trig)
{
	return lis2de12_trigger_anym_tap_set(dev, handler, trig);
}

static int lis2de12_trigger_tap_set(const struct device *dev,
				  sensor_trigger_handler_t handler,
				  const struct sensor_trigger *trig)
{
	return lis2de12_trigger_anym_tap_set(dev, handler, trig);
}

static int lis2de12_trigger_dtap_set(const struct device *dev,
				   sensor_trigger_handler_t handler,
				   const struct sensor_trigger *trig)
{
	return lis2de12_trigger_anym_tap_set(dev, handler, trig);
}

static int lis2de12_start_trigger_int2(const struct device *dev)
{
	struct lis2de12_data *lis2de12 = dev->data;
	const struct lis2de12_config *cfg = dev->config;
	int status = 0;
	uint8_t reg = 0, mask = 0, val = 0;

	setup_int2(dev, true);

	bool has_anyt = (lis2de12->handler_tap != NULL);
	bool has_dtap = (lis2de12->handler_dtap != NULL);
	bool has_anym = (lis2de12->handler_anymotion != NULL);

	/* configure any motion interrupt */
	reg  = cfg->hw.anym_on_int1 ? LIS2DE12_REG_INT1_CFG : LIS2DE12_REG_INT2_CFG;
	val  = (cfg->hw.anym_mode << LIS2DE12_INT_CFG_MODE_SHIFT) | LIS2DE12_ANYM_CFG;
	status = lis2de12->hw_tf->write_reg(dev, reg, val);
	if (status < 0) {
		LOG_ERR("Failed to configure any motion interrupt");
		return status;
	}

	/* latch the interrupt so INTx_SRC holds zone bits until read */
	if (cfg->hw.anym_latch) {
		mask = cfg->hw.anym_on_int1 ? LIS2DE12_EN_LIR_INT1 : LIS2DE12_EN_LIR_INT2;
		status = lis2de12->hw_tf->update_reg(dev, LIS2DE12_REG_CTRL5,
						   mask, mask);
		if (status < 0) {
			LOG_ERR("Failed to latch any motion interrupt");
			return status;
		}
	}

	/* enable any motion detection on int line */
	reg  = cfg->hw.anym_on_int1 ? LIS2DE12_REG_CTRL3 : LIS2DE12_REG_CTRL6;
	mask = cfg->hw.anym_on_int1 ? LIS2DE12_EN_IA_INT1 : LIS2DE12_EN_IA_INT2;
	val  = has_anym ? mask : 0;
	status = lis2de12->hw_tf->update_reg(dev, reg, mask, val);
	if (status < 0) {
		LOG_ERR("Failed to enable any motion detection on int line");
		return status;
	}

	/* configure tap interrupt on all axes */
	reg  = LIS2DE12_REG_CFG_CLICK;
	mask = LIS2DE12_EN_CLICK_XS | LIS2DE12_EN_CLICK_YS | LIS2DE12_EN_CLICK_ZS |
	       LIS2DE12_EN_CLICK_XD | LIS2DE12_EN_CLICK_YD | LIS2DE12_EN_CLICK_ZD;
	val  = (has_anyt ? (LIS2DE12_EN_CLICK_XS | LIS2DE12_EN_CLICK_YS | LIS2DE12_EN_CLICK_ZS) : 0) |
	       (has_dtap ? (LIS2DE12_EN_CLICK_XD | LIS2DE12_EN_CLICK_YD | LIS2DE12_EN_CLICK_ZD) : 0);
	status = lis2de12->hw_tf->update_reg(dev, reg, mask, val);
	if (status < 0) {
		LOG_ERR("Failed to configure tap interrupt");
		return status;
	}

	/* set click detection on int line */
	reg  = cfg->hw.anym_on_int1 ? LIS2DE12_REG_CTRL3 : LIS2DE12_REG_CTRL6;
	mask = cfg->hw.anym_on_int1 ? LIS2DE12_EN_CLICK_INT1 : LIS2DE12_EN_CLICK_INT2;
	val  = (has_anyt || has_dtap) ? mask : 0;
	status = lis2de12->hw_tf->update_reg(dev, reg, mask, val);
	if (status < 0) {
		LOG_ERR("Failed to enable click detection on int line");
		return status;
	}
	return 0;
}

int lis2de12_trigger_set(const struct device *dev,
		       const struct sensor_trigger *trig,
		       sensor_trigger_handler_t handler)
{
	if (trig->type == SENSOR_TRIG_DATA_READY &&
	    trig->chan == SENSOR_CHAN_ACCEL_XYZ) {
		return lis2de12_trigger_drdy_set(dev, trig->chan, handler, trig);
	} else if (trig->type == SENSOR_TRIG_DELTA) {
		return lis2de12_trigger_anym_set(dev, handler, trig);
	} else if (trig->type == SENSOR_TRIG_TAP) {
		return lis2de12_trigger_tap_set(dev, handler, trig);
	} else if (trig->type == SENSOR_TRIG_DOUBLE_TAP) {
		return lis2de12_trigger_dtap_set(dev, handler, trig);
	}

	return -ENOTSUP;
}

/* ODR in Hz, derived from CTRL1; index 9 in low-power mode is 5376 Hz */
static int lis2de12_current_odr_hz(const struct device *dev)
{
	static const uint16_t odr_hz[] = {0, 1, 10, 25, 50, 100, 200, 400, 1620, 1344, 5376};
	struct lis2de12_data *lis2de12 = dev->data;
	uint8_t ctrl1;
	int status = lis2de12->hw_tf->read_reg(dev, LIS2DE12_REG_CTRL1, &ctrl1);

	if (status < 0) {
		return status;
	}

	uint8_t idx = (ctrl1 & LIS2DE12_ODR_MASK) >> LIS2DE12_ODR_SHIFT;

	if (idx == LIS2DE12_ODR_9 && (ctrl1 & LIS2DE12_LP_EN_BIT_MASK)) {
		idx++;
	}

	return odr_hz[idx];
}

/* click timing registers are 1 LSb = 1/ODR; TIME_LIMIT is 7-bit, the rest 8-bit */
static int lis2de12_click_time_set(const struct device *dev, uint8_t reg, int32_t ms,
				 uint8_t max_count)
{
	int odr = lis2de12_current_odr_hz(dev);
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

	struct lis2de12_data *lis2de12 = dev->data;

	return lis2de12->hw_tf->write_reg(dev, reg, (uint8_t)count);
}

int lis2de12_acc_slope_config(const struct device *dev,
			    enum sensor_attribute attr,
			    const struct sensor_value *val)
{
	struct lis2de12_data *lis2de12 = dev->data;
	const struct lis2de12_config *cfg = dev->config;
	int status;

	if (attr == SENSOR_ATTR_SLOPE_TH) {
		uint8_t range_g, reg_val;
		uint32_t slope_th_ums2;

		status = lis2de12->hw_tf->read_reg(dev, LIS2DE12_REG_CTRL4,
						 &reg_val);
		if (status < 0) {
			return status;
		}

		/* fs reg value is in the range 0 (2g) - 3 (16g) */
		range_g = 2 * (1 << ((LIS2DE12_FS_MASK & reg_val)
				      >> LIS2DE12_FS_SHIFT));

		slope_th_ums2 = val->val1 * 1000000 + val->val2;

		/* make sure the provided threshold does not exceed range */
		if ((slope_th_ums2 - 1) > (range_g * SENSOR_G)) {
			return -EINVAL;
		}

		/* 7 bit full range value */
		reg_val = 128 / range_g * (slope_th_ums2 - 1) / SENSOR_G;

		LOG_INF("int2_ths=0x%x range_g=%d ums2=%u", reg_val,
			    range_g, slope_th_ums2 - 1);

		/* Configure threshold for the any motion recognition */
		status = lis2de12->hw_tf->write_reg(dev,
						  cfg->hw.anym_on_int1 ?
							LIS2DE12_REG_INT1_THS :
							LIS2DE12_REG_INT2_THS,
						  reg_val);
	} else if (attr == SENSOR_ATTR_SLOPE_DUR) {
		/*
		 * slope duration is measured in number of samples:
		 * N/ODR where N is the register value
		 */
		if (val->val1 < 0 || val->val1 > 127) {
			return -ENOTSUP;
		}

		LOG_INF("int2_dur=0x%x", val->val1);

		/* Configure time limit for the any motion recognition */
		status = lis2de12->hw_tf->write_reg(dev,
						  cfg->hw.anym_on_int1 ?
							LIS2DE12_REG_INT1_DUR :
							LIS2DE12_REG_INT2_DUR,
						  val->val1);
	} else if ((int)attr == SENSOR_ATTR_LIS2DE12_CLICK_THS) {
		if (val->val1 < 1 || val->val1 > 127) {
			return -EINVAL;
		}
		status = lis2de12->hw_tf->write_reg(dev, LIS2DE12_REG_CFG_CLICK_THS,
						  LIS2DE12_CLICK_LIR | val->val1);
	} else if ((int)attr == SENSOR_ATTR_LIS2DE12_CLICK_TIME_LIMIT_MS) {
		status = lis2de12_click_time_set(dev, LIS2DE12_REG_TIME_LIMIT, val->val1, 127);
	} else if ((int)attr == SENSOR_ATTR_LIS2DE12_CLICK_LATENCY_MS) {
		status = lis2de12_click_time_set(dev, LIS2DE12_REG_TIME_LATENCY, val->val1, 255);
	} else if ((int)attr == SENSOR_ATTR_LIS2DE12_CLICK_WINDOW_MS) {
		status = lis2de12_click_time_set(dev, LIS2DE12_REG_TIME_WINDOW, val->val1, 255);
	} else {
		status = -ENOTSUP;
	}

	return status;
}

static void lis2de12_gpio_int1_callback(const struct device *dev,
				      struct gpio_callback *cb, uint32_t pins)
{
	struct lis2de12_data *lis2de12 =
		CONTAINER_OF(cb, struct lis2de12_data, gpio_int1_cb);

	ARG_UNUSED(pins);

	atomic_set_bit(&lis2de12->trig_flags, TRIGGED_INT1);

	/* int is level triggered so disable until processed */
	setup_int1(lis2de12->dev, false);

#if defined(CONFIG_LIS2DE12_TRIGGER_OWN_THREAD)
	k_sem_give(&lis2de12->gpio_sem);
#elif defined(CONFIG_LIS2DE12_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&lis2de12->work);
#endif
}

static void lis2de12_gpio_int2_callback(const struct device *dev,
				      struct gpio_callback *cb, uint32_t pins)
{
	struct lis2de12_data *lis2de12 =
		CONTAINER_OF(cb, struct lis2de12_data, gpio_int2_cb);

	ARG_UNUSED(pins);

	atomic_set_bit(&lis2de12->trig_flags, TRIGGED_INT2);

	/* int is level triggered so disable until processed */
	setup_int2(lis2de12->dev, false);

#if defined(CONFIG_LIS2DE12_TRIGGER_OWN_THREAD)
	k_sem_give(&lis2de12->gpio_sem);
#elif defined(CONFIG_LIS2DE12_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&lis2de12->work);
#endif
}

static void lis2de12_thread_cb(const struct device *dev)
{
	struct lis2de12_data *lis2de12 = dev->data;
	const struct lis2de12_config *cfg = dev->config;
	int status;

	if (cfg->gpio_drdy.port &&
			unlikely(atomic_test_and_clear_bit(&lis2de12->trig_flags,
			START_TRIG_INT1))) {
		status = lis2de12_start_trigger_int1(dev);

		if (unlikely(status < 0)) {
			LOG_ERR("lis2de12_start_trigger_int1: %d", status);
		}
		return;
	}

	if (cfg->gpio_int.port &&
			unlikely(atomic_test_and_clear_bit(&lis2de12->trig_flags,
			START_TRIG_INT2))) {
		status = lis2de12_start_trigger_int2(dev);

		if (unlikely(status < 0)) {
			LOG_ERR("lis2de12_start_trigger_int2: %d", status);
		}
		return;
	}

	if (cfg->gpio_drdy.port &&
			atomic_test_and_clear_bit(&lis2de12->trig_flags,
			TRIGGED_INT1)) {
		if (likely(lis2de12->handler_drdy != NULL)) {
			lis2de12->handler_drdy(dev, lis2de12->trig_drdy);
		}

		/* Reactivate level triggered interrupt if handler did not
		 * disable itself
		 */
		if (likely(lis2de12->handler_drdy != NULL)) {
			setup_int1(dev, true);
		}

		return;
	}

	if (cfg->gpio_int.port &&
			atomic_test_and_clear_bit(&lis2de12->trig_flags,
			TRIGGED_INT2)) {
		uint8_t reg_val = 0;

		/* if necessary also clears an interrupt to de-assert int line */
		status = lis2de12->hw_tf->read_reg(dev,
						 cfg->hw.anym_on_int1 ?
							LIS2DE12_REG_INT1_SRC :
							LIS2DE12_REG_INT2_SRC,
						 &reg_val);
		if (status < 0) {
			LOG_ERR("clearing interrupt 2 failed: %d", status);
			return;
		}

		if (likely(lis2de12->handler_anymotion != NULL) &&
				(reg_val >> LIS2DE12_INT_CFG_MODE_SHIFT)) {
			lis2de12->handler_anymotion(dev, lis2de12->trig_anymotion);
		}

		/* read click interrupt */
		status = lis2de12->hw_tf->read_reg(dev, LIS2DE12_REG_CLICK_SRC,
						 &reg_val);
		if (status < 0) {
			LOG_ERR("clearing interrupt 2 failed: %d", status);
			return;
		}

		if ((reg_val & (LIS2DE12_CLICK_SRC_SCLICK | LIS2DE12_CLICK_SRC_DCLICK)) != 0) {
			lis2de12->click_src = reg_val;
		}

		if (likely(lis2de12->handler_tap != NULL) &&
				(reg_val & LIS2DE12_CLICK_SRC_SCLICK)) {
			lis2de12->handler_tap(dev, lis2de12->trig_tap);
		}

		if (likely(lis2de12->handler_dtap != NULL) &&
				(reg_val & LIS2DE12_CLICK_SRC_DCLICK)) {
			lis2de12->handler_dtap(dev, lis2de12->trig_dtap);
		}

		/* Reactivate level triggered interrupt if handler did not
		 * disable itself
		 */
		if (lis2de12->handler_anymotion || lis2de12->handler_tap || lis2de12->handler_dtap) {
			setup_int2(dev, true);
		}

		return;
	}
}

#ifdef CONFIG_LIS2DE12_TRIGGER_OWN_THREAD
static void lis2de12_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct lis2de12_data *lis2de12 = p1;

	while (1) {
		k_sem_take(&lis2de12->gpio_sem, K_FOREVER);
		lis2de12_thread_cb(lis2de12->dev);
	}
}
#endif

#ifdef CONFIG_LIS2DE12_TRIGGER_GLOBAL_THREAD
static void lis2de12_work_cb(struct k_work *work)
{
	struct lis2de12_data *lis2de12 =
		CONTAINER_OF(work, struct lis2de12_data, work);

	lis2de12_thread_cb(lis2de12->dev);
}
#endif

int lis2de12_init_interrupt(const struct device *dev)
{
	struct lis2de12_data *lis2de12 = dev->data;
	const struct lis2de12_config *cfg = dev->config;
	int status;

	lis2de12->dev = dev;

#if defined(CONFIG_LIS2DE12_TRIGGER_OWN_THREAD)
	k_sem_init(&lis2de12->gpio_sem, 0, K_SEM_MAX_LIMIT);

	k_thread_create(&lis2de12->thread, lis2de12->thread_stack, CONFIG_LIS2DE12_THREAD_STACK_SIZE,
			lis2de12_thread, lis2de12, NULL, NULL,
			K_PRIO_COOP(CONFIG_LIS2DE12_THREAD_PRIORITY), 0, K_NO_WAIT);
#elif defined(CONFIG_LIS2DE12_TRIGGER_GLOBAL_THREAD)
	k_work_init(&lis2de12->work, lis2de12_work_cb);
#endif

	/*
	 * Setup INT1 (for DRDY) if defined in DT
	 */

	/* setup data ready gpio interrupt */
	if (!gpio_is_ready_dt(&cfg->gpio_drdy)) {
		/* API may return false even when ptr is NULL */
		if (cfg->gpio_drdy.port != NULL) {
			LOG_ERR("GPIO device not ready: %s", cfg->gpio_drdy.port->name);
			return -ENODEV;
		}

		LOG_DBG("gpio_drdy not defined in DT");
		status = 0;
		goto check_gpio_int;
	}

	/* data ready int1 gpio configuration */
	status = gpio_pin_configure_dt(&cfg->gpio_drdy, GPIO_INPUT);
	if (status < 0) {
		LOG_ERR("Could not configure %s.%02u",
			cfg->gpio_drdy.port->name, cfg->gpio_drdy.pin);
		return status;
	}

	gpio_init_callback(&lis2de12->gpio_int1_cb,
			   lis2de12_gpio_int1_callback,
			   BIT(cfg->gpio_drdy.pin));

	status = gpio_add_callback(cfg->gpio_drdy.port, &lis2de12->gpio_int1_cb);
	if (status < 0) {
		LOG_ERR("Could not add gpio int1 callback");
		return status;
	}

	LOG_INF("%s: int1 on %s.%02u", dev->name,
				       cfg->gpio_drdy.port->name,
				       cfg->gpio_drdy.pin);

check_gpio_int:
	/*
	 * Setup Interrupt (for Any Motion) if defined in DT
	 */

	/* setup any motion gpio interrupt */
	if (!gpio_is_ready_dt(&cfg->gpio_int)) {
		/* API may return false even when ptr is NULL */
		if (cfg->gpio_int.port != NULL) {
			LOG_ERR("GPIO device not ready: %s", cfg->gpio_int.port->name);
			return -ENODEV;
		}

		LOG_DBG("gpio_int not defined in DT");
		status = 0;
		goto end;
	}

	/* any motion int2 gpio configuration */
	status = gpio_pin_configure_dt(&cfg->gpio_int, GPIO_INPUT);
	if (status < 0) {
		LOG_ERR("Could not configure %s.%02u",
			cfg->gpio_int.port->name, cfg->gpio_int.pin);
		return status;
	}

	gpio_init_callback(&lis2de12->gpio_int2_cb,
			   lis2de12_gpio_int2_callback,
			   BIT(cfg->gpio_int.pin));

	/* callback is going to be enabled by trigger setting function */
	status = gpio_add_callback(cfg->gpio_int.port, &lis2de12->gpio_int2_cb);
	if (status < 0) {
		LOG_ERR("Could not add gpio int2 callback (%d)", status);
		return status;
	}

	LOG_INF("%s: int2 on %s.%02u", dev->name,
	   cfg->gpio_int.port->name,
	   cfg->gpio_int.pin);

	/* disable interrupt in case of warm (re)boot */
	status = lis2de12->hw_tf->write_reg(dev,
					  cfg->hw.anym_on_int1 ?
						LIS2DE12_REG_INT1_CFG :
						LIS2DE12_REG_INT2_CFG,
					  0);
	if (status < 0) {
		LOG_ERR("Interrupt disable reg write failed (%d)", status);
		return status;
	}
	status = lis2de12->hw_tf->write_reg(dev,
					  LIS2DE12_REG_CFG_CLICK,
					  0);
	if (status < 0) {
		LOG_ERR("Interrupt disable reg write failed (%d)", status);
		return status;
	}

end:
	return status;
}
