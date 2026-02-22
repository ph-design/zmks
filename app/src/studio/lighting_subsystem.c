/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

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

static zmk_studio_Response get_rgb_underglow_state(const zmk_studio_Request *req) {
    LOG_DBG("");
    bool on = false;
    int ret = zmk_rgb_underglow_get_state(&on);

    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_lighting_RgbUnderglowState resp = zmk_lighting_RgbUnderglowState_init_zero;
    resp.on = on;

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
        break;
    }
    case zmk_lighting_SetRgbUnderglowStateRequest_effect_tag:
        ret = zmk_rgb_underglow_select_effect((int)set_req->field.effect);
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
    zmk_rgb_underglow_save_state();
#endif

    /* Backlight state is automatically persisted by zmk_backlight_on/off/set_brt,
     * so no explicit save call is needed here. */

    return LIGHTING_RESPONSE(save_state, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(lighting, save_state, ZMK_STUDIO_RPC_HANDLER_SECURED);
