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

static int lighting_settings_reset(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    int ret = settings_delete("rgb/underglow/state");
    if (ret < 0 && ret != -ENOENT) {
        return ret;
    }
#endif

    return 0;
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(lighting, lighting_settings_reset);
