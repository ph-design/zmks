/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/settings/settings.h>
#include <pb_encode.h>
#include <zmk/studio/rpc.h>

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
#include <zmk/backlight.h>
#endif

ZMK_RPC_SUBSYSTEM(lighting)

#define LIGHTING_RESPONSE(type, ...) ZMK_RPC_RESPONSE(lighting, type, __VA_ARGS__)

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)

static const char *const effect_names[] = {
    "Solid",
    "Breathing",
    "Rainbow",
    "Reactive",
    "Wave",
    "Knight",
    "Twinkle",
    "Gradient",
    "Sparkle",
    "Ripple",
    "Alphas Mods",
    "Raindrops",
    "Reactive Wide",
    "Reactive Nexus",
    "Typing Heatmap",
};

BUILD_ASSERT(ARRAY_SIZE(effect_names) == 15,
             "effect_names array size must match UNDERGLOW_EFFECT_NUMBER");

static bool encode_effect_names(pb_ostream_t *stream, const pb_field_t *field,
                                void *const *arg) {
    int count = zmk_rgb_underglow_get_effect_count();
    if (count > (int)ARRAY_SIZE(effect_names)) {
        count = (int)ARRAY_SIZE(effect_names);
    }
    for (int i = 0; i < count; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_string(stream, (const pb_byte_t *)effect_names[i],
                              strlen(effect_names[i]))) {
            return false;
        }
    }
    return true;
}

static zmk_studio_Response get_rgb_underglow_state(const zmk_studio_Request *req) {
    LOG_DBG("");
    bool on = false;
    int ret = zmk_rgb_underglow_get_state(&on);

    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    struct zmk_led_hsb hsb = zmk_rgb_underglow_get_hsb();

    zmk_lighting_RgbUnderglowState resp = zmk_lighting_RgbUnderglowState_init_zero;
    resp.on = on;
    resp.has_color = true;
    resp.color.h = hsb.h;
    resp.color.s = hsb.s;
    resp.color.b = hsb.b;
    resp.effect = (uint32_t)zmk_rgb_underglow_get_effect();
    resp.speed = (uint32_t)zmk_rgb_underglow_get_speed();
    resp.effect_count = (uint32_t)zmk_rgb_underglow_get_effect_count();
    resp.effect_names.funcs.encode = encode_effect_names;

    return LIGHTING_RESPONSE(get_rgb_underglow_state, resp);
}

static zmk_studio_Response set_rgb_underglow_state(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_lighting_SetRgbUnderglowStateRequest *set_req =
        &req->subsystem.lighting.request_type.set_rgb_underglow_state;
    int ret = 0;

    switch (set_req->which_field) {
    case zmk_lighting_SetRgbUnderglowStateRequest_on_tag:
        ret = set_req->field.on ? zmk_rgb_underglow_on() : zmk_rgb_underglow_off();
        break;
    case zmk_lighting_SetRgbUnderglowStateRequest_color_tag: {
        struct zmk_led_hsb hsb = {
            .h = (uint16_t)set_req->field.color.h,
            .s = (uint8_t)set_req->field.color.s,
            .b = (uint8_t)set_req->field.color.b,
        };
        ret = zmk_rgb_underglow_set_hsb(hsb);
        if (ret == 0) {
            zmk_rgb_underglow_save_state();
        }
        break;
    }
    case zmk_lighting_SetRgbUnderglowStateRequest_effect_tag:
        ret = zmk_rgb_underglow_select_effect((int)set_req->field.effect);
        break;
    case zmk_lighting_SetRgbUnderglowStateRequest_speed_tag:
        ret = zmk_rgb_underglow_set_speed((int)set_req->field.speed);
        break;
    default:
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    if (ret < 0) {
        return LIGHTING_RESPONSE(set_rgb_underglow_state, false);
    }

    return LIGHTING_RESPONSE(set_rgb_underglow_state, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(lighting, get_rgb_underglow_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, set_rgb_underglow_state, ZMK_STUDIO_RPC_HANDLER_SECURED);

#endif /* CONFIG_ZMK_RGB_UNDERGLOW */

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)

static zmk_studio_Response get_backlight_state(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_lighting_BacklightState resp = zmk_lighting_BacklightState_init_zero;
    resp.on = zmk_backlight_is_on();
    resp.brightness = zmk_backlight_get_brt();

    return LIGHTING_RESPONSE(get_backlight_state, resp);
}

static zmk_studio_Response set_backlight_state(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_lighting_SetBacklightStateRequest *set_req =
        &req->subsystem.lighting.request_type.set_backlight_state;
    int ret = 0;

    switch (set_req->which_field) {
    case zmk_lighting_SetBacklightStateRequest_on_tag:
        ret = set_req->field.on ? zmk_backlight_on() : zmk_backlight_off();
        break;
    case zmk_lighting_SetBacklightStateRequest_brightness_tag:
        ret = zmk_backlight_set_brt((uint8_t)set_req->field.brightness);
        break;
    default:
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    if (ret < 0) {
        return LIGHTING_RESPONSE(set_backlight_state, false);
    }

    return LIGHTING_RESPONSE(set_backlight_state, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(lighting, get_backlight_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, set_backlight_state, ZMK_STUDIO_RPC_HANDLER_SECURED);

#endif /* CONFIG_ZMK_BACKLIGHT */

static zmk_studio_Response save_state(const zmk_studio_Request *req) {
    LOG_DBG("");

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    int ret = zmk_rgb_underglow_save_state();
    if (ret < 0) {
        LOG_ERR("Failed to save RGB underglow state: %d", ret);
        return LIGHTING_RESPONSE(save_state, false);
    }
#endif

    return LIGHTING_RESPONSE(save_state, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(lighting, save_state, ZMK_STUDIO_RPC_HANDLER_SECURED);

/* ------------------------------------------------------------------ */
/*  Layer LED Colors RPC handlers                                     */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER) && IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)

#include <zmk/rgb_underglow_layer.h>

struct layer_led_encode_state {
    uint8_t layer_count;
};

static bool encode_layer_led_binding(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    uint8_t layer_id = *(uint8_t *)*arg;
    int rgblayer = zmk_rgbmap_id(layer_id);
    if (rgblayer < 0) {
        return true;
    }

    uint8_t key_count = zmk_rgb_layer_key_count();
    for (uint8_t k = 0; k < key_count; k++) {
        uint32_t color;
        if (zmk_rgb_layer_get_color(layer_id, k, &color) < 0) {
            continue;
        }

        zmk_lighting_LayerLedBinding binding = {
            .key_position = k,
            .color = color,
        };

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, zmk_lighting_LayerLedBinding_fields, &binding)) {
            return false;
        }
    }
    return true;
}

static bool encode_layer_led_configs(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    uint8_t layer_count = zmk_rgb_layer_count();

    for (uint8_t l = 0; l < layer_count; l++) {
        int layer_id = zmk_rgb_layer_id(l);
        if (layer_id < 0) {
            continue;
        }

        uint8_t lid = (uint8_t)layer_id;
        zmk_lighting_LayerLedConfig config = zmk_lighting_LayerLedConfig_init_zero;
        config.layer_id = lid;
        config.bindings.funcs.encode = encode_layer_led_binding;
        config.bindings.arg = &lid;

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, zmk_lighting_LayerLedConfig_fields, &config)) {
            return false;
        }
    }
    return true;
}

static zmk_studio_Response get_layer_led_colors(const zmk_studio_Request *req) {
    LOG_DBG("");

    zmk_lighting_GetLayerLedColorsResponse resp =
        zmk_lighting_GetLayerLedColorsResponse_init_zero;
    resp.key_count = zmk_rgb_layer_key_count();
    resp.layer_count = zmk_rgb_layer_count();
    resp.layers.funcs.encode = encode_layer_led_configs;
    resp.enabled = zmk_rgb_layer_is_enabled();

    return LIGHTING_RESPONSE(get_layer_led_colors, resp);
}

static zmk_studio_Response set_layer_led_binding(const zmk_studio_Request *req) {
    LOG_DBG("");

    const zmk_lighting_SetLayerLedBindingRequest *r =
        &req->subsystem.lighting.request_type.set_layer_led_binding;

    int ret = zmk_rgb_layer_set_binding((uint8_t)r->layer_id, (uint8_t)r->key_position, r->color);
    if (ret == 0) {
        raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
            .notification = ZMK_RPC_NOTIFICATION(keymap, unsaved_changes_status_changed, true)});
    }
    return LIGHTING_RESPONSE(set_layer_led_binding, ret == 0);
}

static zmk_studio_Response save_layer_led_state(const zmk_studio_Request *req) {
    LOG_DBG("");

    int ret = zmk_rgb_layer_save();
    if (ret < 0) {
        LOG_ERR("Failed to save layer LED state: %d", ret);
    }

    /* Only persist CapsLock override if the user actually modified it;
     * otherwise we would write default values that activate the override
     * on next boot and discard the devicetree colors. */
    bool caps_en;
    uint32_t caps_off, caps_on;
    uint8_t caps_pos;
    zmk_capslock_indicator_get_state(&caps_en, &caps_off, &caps_on, &caps_pos);
    if (caps_off != 0 || caps_on != 0 || !caps_en) {
        ret |= zmk_capslock_indicator_save();
    }
    return LIGHTING_RESPONSE(save_layer_led_state, ret == 0);
}

static zmk_studio_Response set_layer_led_enabled(const zmk_studio_Request *req) {
    LOG_DBG("");

    const zmk_lighting_SetLayerLedEnabledRequest *r =
        &req->subsystem.lighting.request_type.set_layer_led_enabled;
    int ret = zmk_rgb_layer_set_enabled(r->enabled);
    if (ret == 0) {
        raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
            .notification = ZMK_RPC_NOTIFICATION(keymap, unsaved_changes_status_changed, true)});
    }
    return LIGHTING_RESPONSE(set_layer_led_enabled, ret == 0);
}

ZMK_RPC_SUBSYSTEM_HANDLER(lighting, get_layer_led_colors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, set_layer_led_binding, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, save_layer_led_state, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, set_layer_led_enabled, ZMK_STUDIO_RPC_HANDLER_SECURED);

/* ------------------------------------------------------------------ */
/*  CapsLock Indicator RPC handlers                                   */
/* ------------------------------------------------------------------ */

static zmk_studio_Response get_caps_lock_indicator(const zmk_studio_Request *req) {
    LOG_DBG("");

    bool enabled;
    uint32_t off_color, on_color;
    uint8_t key_pos;
    zmk_capslock_indicator_get_state(&enabled, &off_color, &on_color, &key_pos);

    zmk_lighting_CapsLockIndicatorState resp = zmk_lighting_CapsLockIndicatorState_init_zero;
    resp.enabled = enabled;
    resp.off_color = off_color;
    resp.on_color = on_color;
    resp.key_position = key_pos;

    return LIGHTING_RESPONSE(get_caps_lock_indicator, resp);
}

static zmk_studio_Response set_caps_lock_indicator(const zmk_studio_Request *req) {
    LOG_DBG("");

    const zmk_lighting_SetCapsLockIndicatorRequest *r =
        &req->subsystem.lighting.request_type.set_caps_lock_indicator;
    int ret = 0;

    switch (r->which_field) {
    case zmk_lighting_SetCapsLockIndicatorRequest_enabled_tag:
        ret = zmk_capslock_indicator_set_enabled(r->field.enabled);
        break;
    case zmk_lighting_SetCapsLockIndicatorRequest_off_color_tag:
        ret = zmk_capslock_indicator_set_off_color(r->field.off_color);
        break;
    case zmk_lighting_SetCapsLockIndicatorRequest_on_color_tag:
        ret = zmk_capslock_indicator_set_on_color(r->field.on_color);
        break;
    default:
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    if (ret == 0) {
        raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
            .notification = ZMK_RPC_NOTIFICATION(keymap, unsaved_changes_status_changed, true)});
    }
    return LIGHTING_RESPONSE(set_caps_lock_indicator, ret == 0);
}

ZMK_RPC_SUBSYSTEM_HANDLER(lighting, get_caps_lock_indicator, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, set_caps_lock_indicator, ZMK_STUDIO_RPC_HANDLER_SECURED);

#endif /* CONFIG_EXPERIMENTAL_RGB_LAYER && CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE */

static int lighting_settings_reset(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    int ret = settings_delete("rgb/underglow/state");
    if (ret < 0 && ret != -ENOENT) {
        return ret;
    }
#endif

#if IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER) && IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
    int layer_ret = zmk_rgb_layer_settings_reset();
    if (layer_ret < 0) {
        return layer_ret;
    }
    int caps_ret = zmk_capslock_indicator_settings_reset();
    if (caps_ret < 0) {
        return caps_ret;
    }
#endif

    return 0;
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(lighting, lighting_settings_reset);
