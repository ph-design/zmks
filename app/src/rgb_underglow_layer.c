/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/rgb_underglow_layer.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/peripheral_layers.h>
#endif

#define DT_DRV_COMPAT zmk_underglow_layer
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define UNDERGLOW_LAYER_ENABLED
#define LAYER_ID(node) DT_PROP(node, layer_id)
#define FADE_DELAY(node) DT_PROP(node, fade_delay)

#define TRANSFORMED_RGB_LAYER(node)                                                                \
    {COND_CODE_1(DT_NODE_HAS_PROP(node, bindings),                                                 \
                 (LISTIFY(DT_PROP_LEN(node, bindings), ZMK_RGBMAP_EXTRACT_BINDING, (, ), node)),   \
                 ())}

#define RGBMAP_VAR(_name, _opts)                                                                   \
    static _opts struct zmk_behavior_binding _name[ZMK_RGBMAP_LAYERS_LEN][ZMK_KEYMAP_LEN] = {      \
        DT_INST_FOREACH_CHILD_STATUS_OKAY_SEP(0, TRANSFORMED_RGB_LAYER, (, ))};

RGBMAP_VAR(zmk_rgbmap, COND_CODE_1(IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE), (), (const)))

/* ------------------------------------------------------------------ */
/*  Transform vs legacy pixel-lookup                                  */
/* ------------------------------------------------------------------ */
#if DT_INST_NODE_HAS_PROP(0, transform)
/* New path: rgb_transform node provides map[] and optional led-positions[] */
#define RGB_TRANSFORM_NODE DT_INST_PHANDLE(0, transform)

static const int pixel_lookup_table[] = DT_PROP(RGB_TRANSFORM_NODE, map);

#if DT_NODE_HAS_PROP(RGB_TRANSFORM_NODE, led_positions)
#define RGB_HAS_POSITIONS 1
static const int led_pos_raw[] = DT_PROP(RGB_TRANSFORM_NODE, led_positions);
#define LED_POS_RAW_LEN DT_PROP_LEN(RGB_TRANSFORM_NODE, led_positions)
#else
#define RGB_HAS_POSITIONS 0
#endif /* led_positions */

#elif DT_INST_NODE_HAS_PROP(0, pixel_lookup)
/* Legacy path: pixel-lookup on underglow-layer node itself */
static const int pixel_lookup_table[] = DT_INST_PROP(0, pixel_lookup);
#define RGB_HAS_POSITIONS 0

#else
#error "underglow-layer requires either 'transform' phandle or 'pixel-lookup' array"
#endif

static int zmk_rgbmap_ids[ZMK_RGBMAP_LAYERS_LEN] = {DT_INST_FOREACH_CHILD_SEP(0, LAYER_ID, (, ))};
static int zmk_rgbmap_fds[ZMK_RGBMAP_LAYERS_LEN] = {DT_INST_FOREACH_CHILD_SEP(0, FADE_DELAY, (, ))};

const int rgb_pixel_lookup(int idx) { return pixel_lookup_table[idx]; };

const int zmk_rgbmap_id(uint8_t layer) {
    for (uint8_t i = 0; i < ZMK_RGBMAP_LAYERS_LEN; i++) {
        if (zmk_rgbmap_ids[i] == layer) {
            return i;
        }
    }
    return -1;
}

const int zmk_rgbmap_fade_delay(uint8_t layer) { return zmk_rgbmap_fds[zmk_rgbmap_id(layer)]; }

const struct zmk_behavior_binding *rgb_underglow_get_bindings(uint8_t layer) {
    int rgblayer = zmk_rgbmap_id(layer);
    if (rgblayer == -1) {
        return NULL;
    } else {
        return zmk_rgbmap[rgblayer];
    }
}

uint8_t rgb_underglow_top_layer_with_state(uint32_t state_to_test) {
    for (uint8_t layer = ZMK_KEYMAP_LAYERS_LEN - 1; layer > 0; layer--) {
        if ((state_to_test & (BIT(layer))) == (BIT(layer)) || layer == 0) {
            return layer;
        }
    }
    // return default layer (0)
    return 0;
}

uint8_t rgb_underglow_top_layer(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    return peripheral_highest_layer_active();
#else
    return zmk_keymap_highest_layer_active();
#endif
}

/* ------------------------------------------------------------------ */
/*  Coordinate access (led-positions from rgb_transform)              */
/* ------------------------------------------------------------------ */

bool rgb_has_led_positions(void) {
#if RGB_HAS_POSITIONS
    return true;
#else
    return false;
#endif
}

int rgb_led_positions_count(void) {
#if RGB_HAS_POSITIONS
    return LED_POS_RAW_LEN / 2;
#else
    return 0;
#endif
}

int rgb_led_position_raw_x(int binding_idx) {
#if RGB_HAS_POSITIONS
    int i = binding_idx * 2;
    if (i < 0 || i >= LED_POS_RAW_LEN)
        return 0;
    return led_pos_raw[i];
#else
    return 0;
#endif
}

int rgb_led_position_raw_y(int binding_idx) {
#if RGB_HAS_POSITIONS
    int i = binding_idx * 2 + 1;
    if (i < 0 || i >= LED_POS_RAW_LEN)
        return 0;
    return led_pos_raw[i];
#else
    return 0;
#endif
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
