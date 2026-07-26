/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/devicetree.h>

struct zmk_behavior_binding;

#define ZMK_COMBOS_UTIL_ONE(n) +1

#define ZMK_COMBOS_LEN                                                                             \
    COND_CODE_1(DT_HAS_COMPAT_STATUS_OKAY(zmk_combos),                                             \
                (0 DT_FOREACH_CHILD_STATUS_OKAY(DT_INST(0, zmk_combos), ZMK_COMBOS_UTIL_ONE)),     \
                (0))


struct zmk_combo_pub_config {
    int32_t timeout_ms;
    int32_t require_prior_idle_ms;
    bool slow_release;
    uint32_t layer_mask;
};

struct zmk_combo_full_config {
    struct zmk_combo_pub_config scalar;
    const int32_t *key_positions;
    uint8_t key_position_len;
    const struct zmk_behavior_binding *behavior;
};

size_t zmk_combo_count(void);

int zmk_combo_get_config(uint16_t index, struct zmk_combo_pub_config *out);

int zmk_combo_set_config(uint16_t index, const struct zmk_combo_pub_config *in);

int zmk_combo_set_full_config(uint16_t index, const struct zmk_combo_full_config *in);

const struct zmk_behavior_binding *zmk_combo_get_behavior_binding_at_idx(uint16_t index);

size_t zmk_combo_get_key_positions_at_idx(uint16_t index, const int32_t **positions);

int zmk_combo_save_all(void);

int zmk_combo_settings_reset(void);

int zmk_combo_reload_from_settings(void);
