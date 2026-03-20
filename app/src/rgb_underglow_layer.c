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

#include <zephyr/settings/settings.h>

#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/rgb_underglow_layer.h>
#include <zmk/event_manager.h>
#include <zmk/events/underglow_color_changed.h>

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

/* ------------------------------------------------------------------ */
/*  Runtime layer color modification + settings persistence           */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)

#define RGB_LAYER_SETTINGS_KEY "rgb/layer/%d"

int zmk_rgb_layer_get_color(uint8_t layer_id, uint8_t key_pos, uint32_t *color) {
    int rgblayer = zmk_rgbmap_id(layer_id);
    if (rgblayer < 0 || key_pos >= ZMK_KEYMAP_LEN) {
        return -EINVAL;
    }

    *color = zmk_rgbmap[rgblayer][key_pos].param1;
    return 0;
}

int zmk_rgb_layer_set_binding(uint8_t layer_id, uint8_t key_pos, uint32_t color) {
    int rgblayer = zmk_rgbmap_id(layer_id);
    if (rgblayer < 0 || key_pos >= ZMK_KEYMAP_LEN) {
        return -EINVAL;
    }

    zmk_rgbmap[rgblayer][key_pos].param1 = color;

    raise_zmk_underglow_color_changed(
        (struct zmk_underglow_color_changed){.layers = BIT(layer_id), .wakeup = true});

    return 0;
}

int zmk_rgb_layer_save(void) {
    char key[20];

    for (int l = 0; l < ZMK_RGBMAP_LAYERS_LEN; l++) {
        uint32_t colors[ZMK_KEYMAP_LEN];
        for (int k = 0; k < ZMK_KEYMAP_LEN; k++) {
            colors[k] = zmk_rgbmap[l][k].param1;
        }

        snprintf(key, sizeof(key), RGB_LAYER_SETTINGS_KEY, l);
        int ret = settings_save_one(key, colors, sizeof(colors));
        if (ret < 0) {
            LOG_ERR("Failed to save RGB layer %d: %d", l, ret);
            return ret;
        }
    }

    return 0;
}

uint8_t zmk_rgb_layer_count(void) { return ZMK_RGBMAP_LAYERS_LEN; }

uint8_t zmk_rgb_layer_key_count(void) { return ZMK_KEYMAP_LEN; }

int zmk_rgb_layer_id(uint8_t rgblayer_idx) {
    if (rgblayer_idx >= ZMK_RGBMAP_LAYERS_LEN) {
        return -EINVAL;
    }
    return zmk_rgbmap_ids[rgblayer_idx];
}

static int rgb_layer_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "layer", &next) && next) {
        unsigned int layer_idx = atoi(next);
        if (layer_idx >= ZMK_RGBMAP_LAYERS_LEN) {
            LOG_WRN("RGB layer index %u out of range", layer_idx);
            return -EINVAL;
        }

        uint32_t colors[ZMK_KEYMAP_LEN];
        int rc = read_cb(cb_arg, colors, sizeof(colors));
        if (rc <= 0) {
            LOG_ERR("Failed to read RGB layer %u settings (err %d)", layer_idx, rc);
            return rc;
        }

        for (int k = 0; k < ZMK_KEYMAP_LEN; k++) {
            zmk_rgbmap[layer_idx][k].param1 = colors[k];
        }

        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_layer, "rgb/layer", NULL, rgb_layer_settings_set, NULL, NULL);

int zmk_rgb_layer_settings_reset(void) {
    char key[20];
    for (int l = 0; l < ZMK_RGBMAP_LAYERS_LEN; l++) {
        snprintf(key, sizeof(key), RGB_LAYER_SETTINGS_KEY, l);
        int ret = settings_delete(key);
        if (ret < 0 && ret != -ENOENT) {
            return ret;
        }
    }
    return 0;
}

#endif /* CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE */

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
