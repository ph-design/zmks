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
#include "lis2de12.h"
#include <zephyr/logging/log.h>

#if DT_ANY_INST_ON_BUS_STATUS_OKAY(spi)

LOG_MODULE_DECLARE(lis2de12, CONFIG_SENSOR_LOG_LEVEL);


#define LIS2DE12_SPI_READ_BIT		BIT(7)
#define LIS2DE12_SPI_AUTOINC		BIT(6)
#define LIS2DE12_SPI_ADDR_MASK		BIT_MASK(6)

static int lis2de12_raw_read(const struct device *dev, uint8_t reg_addr,
			    uint8_t *value, uint8_t len)
{
	const struct lis2de12_config *cfg = dev->config;
	uint8_t buffer_tx[2] = { reg_addr | LIS2DE12_SPI_READ_BIT, 0 };
	const struct spi_buf tx_buf = {
			.buf = buffer_tx,
			.len = 2,
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1
	};
	const struct spi_buf rx_buf[2] = {
		{
			.buf = NULL,
			.len = 1,
		},
		{
			.buf = value,
			.len = len,
		}
	};
	const struct spi_buf_set rx = {
		.buffers = rx_buf,
		.count = 2
	};


	if (len > 64) {
		return -EIO;
	}

	if (len > 1) {
		buffer_tx[0] |= LIS2DE12_SPI_AUTOINC;
	}

	if (spi_transceive_dt(&cfg->bus_cfg.spi, &tx, &rx)) {
		return -EIO;
	}

	return 0;
}

static int lis2de12_raw_write(const struct device *dev, uint8_t reg_addr,
			     uint8_t *value, uint8_t len)
{
	const struct lis2de12_config *cfg = dev->config;
	uint8_t buffer_tx[1] = { reg_addr & ~LIS2DE12_SPI_READ_BIT };
	const struct spi_buf tx_buf[2] = {
		{
			.buf = buffer_tx,
			.len = 1,
		},
		{
			.buf = value,
			.len = len,
		}
	};
	const struct spi_buf_set tx = {
		.buffers = tx_buf,
		.count = 2
	};


	if (len > 64) {
		return -EIO;
	}

	if (len > 1) {
		buffer_tx[0] |= LIS2DE12_SPI_AUTOINC;
	}

	if (spi_write_dt(&cfg->bus_cfg.spi, &tx)) {
		return -EIO;
	}

	return 0;
}

static int lis2de12_spi_read_data(const struct device *dev, uint8_t reg_addr,
				 uint8_t *value, uint8_t len)
{
	return lis2de12_raw_read(dev, reg_addr, value, len);
}

static int lis2de12_spi_write_data(const struct device *dev, uint8_t reg_addr,
				  uint8_t *value, uint8_t len)
{
	return lis2de12_raw_write(dev, reg_addr, value, len);
}

static int lis2de12_spi_read_reg(const struct device *dev, uint8_t reg_addr,
				uint8_t *value)
{
	return lis2de12_raw_read(dev, reg_addr, value, 1);
}

static int lis2de12_spi_write_reg(const struct device *dev, uint8_t reg_addr,
				uint8_t value)
{
	uint8_t tmp_val = value;

	return lis2de12_raw_write(dev, reg_addr, &tmp_val, 1);
}

static int lis2de12_spi_update_reg(const struct device *dev, uint8_t reg_addr,
				  uint8_t mask, uint8_t value)
{
	uint8_t tmp_val;

	lis2de12_raw_read(dev, reg_addr, &tmp_val, 1);
	tmp_val = (tmp_val & ~mask) | (value & mask);

	return lis2de12_raw_write(dev, reg_addr, &tmp_val, 1);
}

static const struct lis2de12_transfer_function lis2de12_spi_transfer_fn = {
	.read_data = lis2de12_spi_read_data,
	.write_data = lis2de12_spi_write_data,
	.read_reg  = lis2de12_spi_read_reg,
	.write_reg  = lis2de12_spi_write_reg,
	.update_reg = lis2de12_spi_update_reg,
};

int lis2de12_spi_init(const struct device *dev)
{
	struct lis2de12_data *data = dev->data;
	const struct lis2de12_config *cfg = dev->config;

	data->hw_tf = &lis2de12_spi_transfer_fn;

	if (!spi_is_ready_dt(&cfg->bus_cfg.spi)) {
		LOG_ERR("SPI bus device is not ready: %s", cfg->bus_cfg.spi.bus->name);
		return -ENODEV;
	}

	return 0;
}
#endif /* DT_ANY_INST_ON_BUS_STATUS_OKAY(spi) */
