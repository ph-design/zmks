/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ZMK_BATTERY_CRITICAL_PERCENT 5

bool zmk_battery_is_valid(void);

uint8_t zmk_battery_state_of_charge(void);
