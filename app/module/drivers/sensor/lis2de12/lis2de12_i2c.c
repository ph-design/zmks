/* ST Microelectronics LIS2DE12 3-axis accelerometer driver
 *
 * Copyright (c) 2020 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Datasheet:
 * https://www.st.com/resource/en/datasheet/lis2de12.pdf
 */

#define DT_DRV_COMPAT st_lis2de12

#include <string.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "lis2de12.h"

#if DT_ANY_INST_ON_BUS_STATUS_OKAY(i2c)

LOG_MODULE_DECLARE(lis2de12, CONFIG_SENSOR_LOG_LEVEL);

static int lis2de12_i2c_read_data(const struct device *dev, uint8_t reg_addr,
				 uint8_t *value, uint8_t len)
{
	const struct lis2de12_config *cfg = dev->config;

	return i2c_burst_read_dt(&cfg->bus_cfg.i2c, reg_addr | LIS2DE12_AUTOINCREMENT_ADDR, value,
				 len);
}

static int lis2de12_i2c_write_data(const struct device *dev, uint8_t reg_addr,
				  uint8_t *value, uint8_t len)
{
	const struct lis2de12_config *cfg = dev->config;

	return i2c_burst_write_dt(&cfg->bus_cfg.i2c, reg_addr | LIS2DE12_AUTOINCREMENT_ADDR, value,
				  len);
}

static int lis2de12_i2c_read_reg(const struct device *dev, uint8_t reg_addr,
				uint8_t *value)
{
	const struct lis2de12_config *cfg = dev->config;

	return i2c_reg_read_byte_dt(&cfg->bus_cfg.i2c, reg_addr, value);
}

static int lis2de12_i2c_write_reg(const struct device *dev, uint8_t reg_addr,
				uint8_t value)
{
	const struct lis2de12_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->bus_cfg.i2c, reg_addr, value);
}

static int lis2de12_i2c_update_reg(const struct device *dev, uint8_t reg_addr,
				  uint8_t mask, uint8_t value)
{
	const struct lis2de12_config *cfg = dev->config;

	return i2c_reg_update_byte_dt(&cfg->bus_cfg.i2c, reg_addr, mask, value);
}

static const struct lis2de12_transfer_function lis2de12_i2c_transfer_fn = {
	.read_data = lis2de12_i2c_read_data,
	.write_data = lis2de12_i2c_write_data,
	.read_reg  = lis2de12_i2c_read_reg,
	.write_reg  = lis2de12_i2c_write_reg,
	.update_reg = lis2de12_i2c_update_reg,
};

int lis2de12_i2c_init(const struct device *dev)
{
	struct lis2de12_data *data = dev->data;
	const struct lis2de12_config *cfg = dev->config;

	if (!device_is_ready(cfg->bus_cfg.i2c.bus)) {
		LOG_ERR("Bus device is not ready: %s", cfg->bus_cfg.i2c.bus->name);
		return -ENODEV;
	}

	data->hw_tf = &lis2de12_i2c_transfer_fn;

	return 0;
}
#endif /* DT_ANY_INST_ON_BUS_STATUS_OKAY(i2c) */
