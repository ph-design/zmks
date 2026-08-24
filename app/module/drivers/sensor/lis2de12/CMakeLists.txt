# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources(lis2de12.c)
zephyr_library_sources(lis2de12_i2c.c)
zephyr_library_sources(lis2de12_spi.c)
zephyr_library_sources_ifdef(CONFIG_LIS2DE12_TRIGGER lis2de12_trigger.c)
