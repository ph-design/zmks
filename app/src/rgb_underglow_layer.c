/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <drivers/behavior.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>
#include <string.h>
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

#define RGB_LAYER_BY_ID(node) [LAYER_ID(node)] = TRANSFORMED_RGB_LAYER(node)

#define RGBMAP_VAR(_name, _opts)                                                                   \
    static _opts struct zmk_behavior_binding _name[ZMK_KEYMAP_LAYERS_LEN][ZMK_KEYMAP_LEN] = {      \
        DT_INST_FOREACH_CHILD_STATUS_OKAY_SEP(0, RGB_LAYER_BY_ID, (, ))};

RGBMAP_VAR(zmk_rgbmap, COND_CODE_1(IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE), (), (const)))

#if DT_INST_NODE_HAS_PROP(0, transform)
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
static const int pixel_lookup_table[] = DT_INST_PROP(0, pixel_lookup);
#define RGB_HAS_POSITIONS 0

#else
#error "underglow-layer requires either 'transform' phandle or 'pixel-lookup' array"
#endif

static int zmk_rgbmap_ids[ZMK_RGBMAP_LAYERS_LEN] = {DT_INST_FOREACH_CHILD_SEP(0, LAYER_ID, (, ))};
static int zmk_rgbmap_fds[ZMK_RGBMAP_LAYERS_LEN] = {DT_INST_FOREACH_CHILD_SEP(0, FADE_DELAY, (, ))};

const int rgb_pixel_lookup(int idx) { return pixel_lookup_table[idx]; };

const int zmk_rgbmap_id(uint8_t layer) { return (layer < ZMK_KEYMAP_LAYERS_LEN) ? layer : -1; }

static int zmk_rgbmap_dt_slot(uint8_t layer) {
    for (uint8_t i = 0; i < ZMK_RGBMAP_LAYERS_LEN; i++) {
        if (zmk_rgbmap_ids[i] == layer) {
            return i;
        }
    }
    return -1;
}

const int zmk_rgbmap_fade_delay(uint8_t layer) {
    int slot = zmk_rgbmap_dt_slot(layer);
    return (slot < 0) ? -1 : zmk_rgbmap_fds[slot];
}

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
    return 0;
}

uint8_t rgb_underglow_top_layer(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    return peripheral_highest_layer_active();
#else
    return zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active());
#endif
}

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


#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)

#define RGB_LAYER_SETTINGS_KEY "rgb/layer/%d"
#define RGB_LAYER_ENABLED_KEY "rgb/layer/en"

static bool layer_led_enabled = true;

bool zmk_rgb_layer_is_enabled(void) { return layer_led_enabled; }

int zmk_rgb_layer_set_enabled(bool enabled) {
    layer_led_enabled = enabled;
    raise_zmk_underglow_color_changed(
        (struct zmk_underglow_color_changed){.layers = 0xFFFFFFFF, .wakeup = true});
    return 0;
}

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

    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        uint32_t colors[ZMK_KEYMAP_LEN];
        bool any = false;
        for (int k = 0; k < ZMK_KEYMAP_LEN; k++) {
            colors[k] = zmk_rgbmap[l][k].param1;
            if (colors[k] != 0) {
                any = true;
            }
        }

        snprintf(key, sizeof(key), RGB_LAYER_SETTINGS_KEY, l);

        int ret = any ? settings_save_one(key, colors, sizeof(colors)) : settings_delete(key);
        if (ret < 0 && ret != -ENOENT) {
            LOG_ERR("Failed to save RGB layer %d: %d", l, ret);
            return ret;
        }
    }

    /* Persist enabled flag */
    uint8_t en = layer_led_enabled ? 1 : 0;
    int ret = settings_save_one(RGB_LAYER_ENABLED_KEY, &en, sizeof(en));
    if (ret < 0) {
        LOG_ERR("Failed to save layer LED enabled: %d", ret);
        return ret;
    }

    return 0;
}

uint8_t zmk_rgb_layer_count(void) { return ZMK_KEYMAP_LAYERS_LEN; }

uint8_t zmk_rgb_layer_key_count(void) { return ZMK_KEYMAP_LEN; }

int zmk_rgb_layer_id(uint8_t rgblayer_idx) {
    if (rgblayer_idx >= ZMK_KEYMAP_LAYERS_LEN) {
        return -EINVAL;
    }
    return rgblayer_idx;
}

static int rgb_layer_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    /* Handler prefix is "rgb/layer". Zephyr strips it, so we receive:
     *   key "rgb/layer/0"  → name = "0"
     *   key "rgb/layer/en" → name = "en"
     */
    if (!name || !name[0]) {
        return -ENOENT;
    }

    if (strcmp(name, "en") == 0) {
        uint8_t en;
        int rc = read_cb(cb_arg, &en, sizeof(en));
        if (rc <= 0) {
            return rc;
        }
        layer_led_enabled = en ? true : false;
        return 0;
    }

    unsigned int layer_idx = atoi(name);
    if (layer_idx >= ZMK_KEYMAP_LAYERS_LEN) {
        LOG_WRN("RGB layer index %u out of range", layer_idx);
        return -EINVAL;
    }

    uint32_t colors[ZMK_KEYMAP_LEN];
    int rc = read_cb(cb_arg, colors, sizeof(colors));
    if (rc <= 0) {
        LOG_ERR("Failed to read RGB layer %u settings (err %d)", layer_idx, rc);
        return rc;
    }
    if ((size_t)rc != sizeof(colors)) {
        LOG_WRN("Discarding RGB layer %u settings of unexpected size %d", layer_idx, rc);
        return 0;
    }

    for (int k = 0; k < ZMK_KEYMAP_LEN; k++) {
        zmk_rgbmap[layer_idx][k].param1 = colors[k];
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_layer, "rgb/layer", NULL, rgb_layer_settings_set, NULL, NULL);

int zmk_rgb_layer_settings_reset(void) {
    char key[20];
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        snprintf(key, sizeof(key), RGB_LAYER_SETTINGS_KEY, l);
        int ret = settings_delete(key);
        if (ret < 0 && ret != -ENOENT) {
            return ret;
        }
    }
    settings_delete(RGB_LAYER_ENABLED_KEY);
    layer_led_enabled = true;
    return 0;
}

static int rgb_layer_seed_bindings(void) {
    const char *color_dev = NULL;

    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN && color_dev == NULL; l++) {
        for (int k = 0; k < ZMK_KEYMAP_LEN; k++) {
            if (zmk_rgbmap[l][k].behavior_dev != NULL) {
                color_dev = zmk_rgbmap[l][k].behavior_dev;
                break;
            }
        }
    }

#if DT_NODE_EXISTS(DT_NODELABEL(ug))
    if (color_dev == NULL) {
        color_dev = DEVICE_DT_NAME(DT_NODELABEL(ug));
    }
#endif

    if (color_dev == NULL) {
        return 0;
    }

    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        for (int k = 0; k < ZMK_KEYMAP_LEN; k++) {
            if (zmk_rgbmap[l][k].behavior_dev == NULL) {
                zmk_rgbmap[l][k].behavior_dev = color_dev;
            }
        }
    }
    return 0;
}

SYS_INIT(rgb_layer_seed_bindings, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE */

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
