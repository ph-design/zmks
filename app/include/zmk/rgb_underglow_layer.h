/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <zmk/keymap.h>

#define ZMK_RGB_CHILD_LEN_PLUS_ONE(node) 1 +

#define ZMK_RGBMAP_LAYERS_LEN                                                                      \
    (DT_FOREACH_CHILD(DT_INST(0, zmk_underglow_layer), ZMK_RGB_CHILD_LEN_PLUS_ONE) 0)

#define ZMK_RGBMAP_EXTRACT_BINDING(idx, drv_inst)                                                  \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(drv_inst, bindings, idx)),                \
        .param1 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(drv_inst, bindings, idx, param1), (0),        \
                              (DT_PHA_BY_IDX(drv_inst, bindings, idx, param1))),                   \
        .param2 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(drv_inst, bindings, idx, param2), (0),        \
                              (DT_PHA_BY_IDX(drv_inst, bindings, idx, param2))),                   \
    }

const int rgb_pixel_lookup(int idx);
const int zmk_rgbmap_id(uint8_t layer);
const int zmk_rgbmap_fade_delay(uint8_t layer);

/* Coordinate-based RGB support (requires zmk,rgb-matrix-transform with led-positions) */
bool rgb_has_led_positions(void);
int rgb_led_positions_count(void);
int rgb_led_position_raw_x(int binding_idx);
int rgb_led_position_raw_y(int binding_idx);

const struct zmk_behavior_binding *rgb_underglow_get_bindings(uint8_t layer);

uint8_t rgb_underglow_top_layer_with_state(uint32_t state_to_test);
uint8_t rgb_underglow_top_layer(void);

/* Runtime layer LED color configuration (requires CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE) */
int zmk_rgb_layer_get_color(uint8_t layer_id, uint8_t key_pos, uint32_t *color);
int zmk_rgb_layer_set_binding(uint8_t layer_id, uint8_t key_pos, uint32_t color);
int zmk_rgb_layer_save(void);
uint8_t zmk_rgb_layer_count(void);
uint8_t zmk_rgb_layer_key_count(void);
int zmk_rgb_layer_id(uint8_t rgblayer_idx);
int zmk_rgb_layer_settings_reset(void);
bool zmk_rgb_layer_is_enabled(void);
int zmk_rgb_layer_set_enabled(bool enabled);

#define ZMK_STATUS_INDICATOR_ANY_LAYER 0xFF

int zmk_capslock_indicator_get_state(bool *enabled, uint32_t *off_color, uint32_t *on_color,
                                     uint8_t *key_pos, uint8_t *layer_id);
int zmk_capslock_indicator_set_enabled(bool enabled);
int zmk_capslock_indicator_set_off_color(uint32_t color);
int zmk_capslock_indicator_set_on_color(uint32_t color);
int zmk_capslock_indicator_set_key_pos(uint8_t key_pos);
int zmk_capslock_indicator_set_layer_id(uint8_t layer_id);
int zmk_capslock_indicator_save(void);
int zmk_capslock_indicator_settings_reset(void);

int zmk_connection_indicator_get_state(bool *enabled, uint32_t *usb_color, uint32_t *bt_color,
                                       uint8_t *key_pos, uint8_t *layer_id);
int zmk_connection_indicator_set_enabled(bool enabled);
int zmk_connection_indicator_set_usb_color(uint32_t color);
int zmk_connection_indicator_set_bt_color(uint32_t color);
int zmk_connection_indicator_set_key_pos(uint8_t key_pos);
int zmk_connection_indicator_set_layer_id(uint8_t layer_id);
int zmk_connection_indicator_save(void);
int zmk_connection_indicator_settings_reset(void);

bool zmk_capslock_indicator_resolve(uint8_t top_layer, uint8_t *out_key_pos, uint32_t *out_color);
bool zmk_connection_indicator_resolve(uint8_t top_layer, uint8_t *out_key_pos, uint32_t *out_color);